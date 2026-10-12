/*
 * pet_status_lamp_web.cpp — 内置网页与 JSON API（档 1 的"App 控制"）
 *
 * 设计取舍：**不依赖任何 ESPHome web_server 的内部 API**。
 *   ESPHome 的 web_server / web_server_idf 在不同版本间改动频繁，
 *   而我们无法在本机编译验证，硬接它的 Handler 会带来版本脆性。
 *   因此这里直接用 ESP-IDF 的 esp_http_server（ESPHome 的 web_server_idf
 *   自己也是这么做的，所以该组件在 ESP32 构建里一定可用）。
 *
 * 端口约定：默认 8080，**不是 80**。
 *   如果你同时启用了 ESPHome 的 `web_server:`（默认 80），
 *   两个端口并存，互不冲突；合二选一时把其中一个换成同一个端口。
 *
 * 认证：Basic Auth，用户名/密码来自 YAML（web_username / web_password）。
 *   同 WiFi 下默认任何人都能开这个网页，所以**默认要求认证**。
 */
#include "pet_status_lamp.h"

namespace esphome::pet_status_lamp {

static const char *const TAG = "pet_lamp.web";

/* ------------------------------------------------------------------ */
/* 工具                                                               */
/* ------------------------------------------------------------------ */

/** 极简 base64（用于 Basic Auth 比对；只做编码，不需要解码） */
static void b64_encode_append(const char *in, std::string *out) {
  static const char *kTable =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  const unsigned char *p = reinterpret_cast<const unsigned char *>(in);
  size_t len = strlen(in);
  for (size_t i = 0; i < len; i += 3) {
    const uint32_t b0 = p[i];
    const uint32_t b1 = (i + 1 < len) ? p[i + 1] : 0u;
    const uint32_t b2 = (i + 2 < len) ? p[i + 2] : 0u;
    const uint32_t triple = (b0 << 16) | (b1 << 8) | b2;
    out->push_back(kTable[(triple >> 18) & 0x3F]);
    out->push_back(kTable[(triple >> 12) & 0x3F]);
    out->push_back((i + 1 < len) ? kTable[(triple >> 6) & 0x3F] : '=');
    out->push_back((i + 2 < len) ? kTable[triple & 0x3F] : '=');
  }
}

static PetStatusLamp *self_of(httpd_req_t *req) {
  return static_cast<PetStatusLamp *>(req->user_ctx);
}

/** 返回 true 表示已认证（或未配置认证） */
static bool auth_ok_(httpd_req_t *req) {
  PetStatusLamp *self = self_of(req);
  if (self == nullptr) {
    return false;
  }
  /* 允许 httpd 自己的 auth 机制（如果用户用 httpd_config 配了） */
  if (httpd_req_get_authz(req) != nullptr) {
    return true;
  }
  const std::string &user = self->web_user();
  const std::string &pass = self->web_pass();
  if (user.empty() && pass.empty()) {
    return true;
  }

  char hdr[192];
  const esp_err_t err =
      httpd_req_get_hdr_value_str(req, "Authorization", hdr, sizeof(hdr));
  if (err != ESP_OK) {
    return false;
  }
  /* 形如 "Basic <base64(user:pass)>" */
  if (strncmp(hdr, "Basic ", 6) != 0) {
    return false;
  }
  const char *encoded = hdr + 6;
  while (*encoded == ' ') {
    ++encoded;
  }
  std::string expected;
  const std::string creds = user + ":" + pass;
  b64_encode_append(creds.c_str(), &expected);
  return expected == encoded;
}

static bool require_auth_(httpd_req_t *req) {
  if (auth_ok_(req)) {
    return true;
  }
  httpd_resp_set_status(req, "401 Unauthorized");
  httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"pet-status-lamp\"");
  httpd_resp_send(req, "unauthorized\n", HTTPD_RESP_USE_STRLEN);
  return false;
}

static esp_err_t send_text_(httpd_req_t *req, const char *ctype,
                            const std::string &body) {
  httpd_resp_set_type(req, ctype);
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  /* 不用 send_chunk：body 在返回前一直存活，一次性发送即可 */
  return httpd_resp_send(req, body.c_str(), (ssize_t)body.size());
}

static bool query_int_(httpd_req_t *req, const char *key, int *out) {
  if (httpd_req_get_url_query_len(req) <= 0) {
    return false;
  }
  char buf[64];
  if (httpd_req_get_url_query_str(req, buf, sizeof(buf)) != ESP_OK) {
    return false;
  }
  char val[24];
  if (httpd_query_key_value(buf, key, val, sizeof(val)) != ESP_OK) {
    return false;
  }
  *out = atoi(val);
  return true;
}

/** 读取整个 POST body 到字符串；为空或过长返回 false */
static bool read_body_(httpd_req_t *req, std::string *out, size_t max_len) {
  const size_t len = req->content_len;
  if (len == 0u || len > max_len) {
    return false;
  }
  out->resize(len);
  size_t got = 0;
  while (got < len) {
    const int r = httpd_req_recv(req, &(*out)[got], len - got);
    if (r <= 0) {
      if (r == HTTPD_SOCK_ERR_TIMEOUT) {
        continue;
      }
      return false;
    }
    got += (size_t)r;
  }
  return true;
}

/**
 * 极简 JSON 取值：从 {"key":123} / {"key":"str"} 里取一个键。
 * 不引入 JSON 库的原因：请求体字段固定且极少，自己解析更小、更可控。
 */
static bool json_get_string(const std::string &json, const char *key,
                            std::string *out) {
  const std::string k = std::string("\"") + key + "\"";
  const size_t kpos = json.find(k);
  if (kpos == std::string::npos) {
    return false;
  }
  size_t p = json.find(':', kpos + k.size());
  if (p == std::string::npos) {
    return false;
  }
  ++p;
  while (p < json.size() && (json[p] == ' ' || json[p] == '\t')) {
    ++p;
  }
  if (p >= json.size() || json[p] != '"') {
    return false;
  }
  ++p;
  out->clear();
  while (p < json.size() && json[p] != '"') {
    if (json[p] == '\\' && p + 1 < json.size()) {
      ++p;
      switch (json[p]) {
        case 'n': out->push_back('\n'); break;
        case 'r': out->push_back('\r'); break;
        case 't': out->push_back('\t'); break;
        case '"': out->push_back('"'); break;
        case '\\': out->push_back('\\'); break;
        default: out->push_back(json[p]); break;
      }
    } else {
      out->push_back(json[p]);
    }
    ++p;
  }
  return true;
}

static bool json_get_number(const std::string &json, const char *key, long *out) {
  const std::string k = std::string("\"") + key + "\"";
  const size_t kpos = json.find(k);
  if (kpos == std::string::npos) {
    return false;
  }
  size_t p = json.find(':', kpos + k.size());
  if (p == std::string::npos) {
    return false;
  }
  ++p;
  while (p < json.size() && (json[p] == ' ' || json[p] == '\t')) {
    ++p;
  }
  const bool neg = (p < json.size() && json[p] == '-');
  if (neg) {
    ++p;
  }
  if (p >= json.size() || json[p] < '0' || json[p] > '9') {
    return false;
  }
  long v = 0;
  while (p < json.size() && json[p] >= '0' && json[p] <= '9') {
    v = v * 10 + (json[p] - '0');
    ++p;
  }
  *out = neg ? -v : v;
  return true;
}

/* ------------------------------------------------------------------ */
/* GET 处理器                                                          */
/* ------------------------------------------------------------------ */

esp_err_t pet_http_index_(httpd_req_t *req) {
  if (!require_auth_(req)) {
    return ESP_OK;
  }
  return send_text_(req, "text/html; charset=utf-8",
                    std::string(PET_UI_HTML));
}

esp_err_t pet_http_state_(httpd_req_t *req) {
  if (!require_auth_(req)) {
    return ESP_OK;
  }
  PetStatusLamp *self = self_of(req);
  return send_text_(req, "application/json", self->json_state());
}

esp_err_t pet_http_devices_(httpd_req_t *req) {
  if (!require_auth_(req)) {
    return ESP_OK;
  }
  PetStatusLamp *self = self_of(req);
  return send_text_(req, "application/json", self->json_devices());
}

esp_err_t pet_http_settings_(httpd_req_t *req) {
  if (!require_auth_(req)) {
    return ESP_OK;
  }
  PetStatusLamp *self = self_of(req);
  return send_text_(req, "application/json", self->json_settings());
}

/* ------------------------------------------------------------------ */
/* POST 处理器：/api/pet 与 /api/light                                 */
/* ------------------------------------------------------------------ */

esp_err_t pet_http_post_(httpd_req_t *req) {
  if (!require_auth_(req)) {
    return ESP_OK;
  }
  PetStatusLamp *self = self_of(req);
  const char *uri = req->uri;

  std::string body;
  if (!read_body_(req, &body, 512u)) {
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_send(req, "empty or oversized body\n", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
  }

  std::string action;
  if (!json_get_string(body, "action", &action)) {
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_send(req, "missing action\n", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
  }

  const bool is_light =
      (strncmp(uri, "/api/light", 10) == 0);
  std::string reply = "{\"ok\":true";

  if (is_light) {
    if (action == "power") {
      bool on = false;
      std::string v;
      if (json_get_string(body, "on", &v)) {
        on = (v == "true" || v == "1");
      } else {
        long n = 0;
        if (json_get_number(body, "on", &n)) {
          on = (n != 0);
        }
      }
      self->cmd_set_power(on);
    } else if (action == "brightness") {
      long n = 0;
      if (json_get_number(body, "value", &n)) {
        self->cmd_set_brightness((uint8_t)n);
      }
    } else if (action == "color_temp") {
      long n = 0;
      if (json_get_number(body, "value", &n)) {
        self->cmd_set_color_temp((uint32_t)n);
      }
    } else if (action == "rgb") {
      long n = 0;
      if (json_get_number(body, "value", &n)) {
        self->cmd_set_rgb((uint32_t)n);
      }
    } else if (action == "mode") {
      long n = 0;
      if (json_get_number(body, "value", &n)) {
        self->cmd_set_mode((uint8_t)n);
      }
    } else {
      reply = "{\"ok\":false,\"error\":\"unknown light action\"}";
    }
  } else {
    /* ---- /api/pet ---- */
    if (action == "rename") {
      std::string name;
      long id = 0;
      long index = -1;
      const bool has_id = json_get_number(body, "device_id", &id);
      const bool has_index = json_get_number(body, "index", &index);
      json_get_string(body, "name", &name);   /* 允许空串 = 恢复默认名 */
      uint32_t target = (uint32_t)id;
      if (!has_id && has_index && index >= 0 &&
          (uint8_t)index < self->device_count()) {
        target = self->device_at((uint8_t)index)->device_id;
      }
      if (target != 0u) {
        if (!self->cmd_rename(target, name)) {
          reply = "{\"ok\":false,\"error\":\"device not bound\"}";
        }
      } else {
        reply = "{\"ok\":false,\"error\":\"need device_id or index\"}";
      }
    } else if (action == "focus") {
      long n = -1;
      json_get_number(body, "index", &n);
      if (!self->cmd_set_focus((int)n)) {
        reply = "{\"ok\":false,\"error\":\"invalid index\"}";
      }
    } else if (action == "remove") {
      long id = 0;
      if (json_get_number(body, "device_id", &id) &&
          self->cmd_remove((uint32_t)id)) {
        /* ok */
      } else {
        reply = "{\"ok\":false,\"error\":\"device not bound\"}";
      }
    } else if (action == "max_devices") {
      long n = 0;
      if (json_get_number(body, "value", &n)) {
        if (!self->cmd_set_max_devices((uint8_t)n)) {
          reply = "{\"ok\":false,\"error\":\"clamped to 1..16\"}";
        }
      } else {
        reply = "{\"ok\":false,\"error\":\"missing value\"}";
      }
    } else if (action == "pairing") {
      self->cmd_enter_pairing();
    } else if (action == "clear_bindings") {
      self->cmd_clear_bindings();
    } else if (action == "night_enabled") {
      long n = 0;
      const bool has = json_get_number(body, "value", &n);
      bool on = (n != 0);
      std::string v;
      if (!has && json_get_string(body, "value", &v)) {
        on = (v == "true" || v == "1");
      }
      self->cmd_set_night_enabled(on);
    } else if (action == "night_window") {
      long s = -1, e = -1;
      json_get_number(body, "start", &s);
      json_get_number(body, "end", &e);
      if (s >= 0 && s <= 23 && e >= 0 && e <= 23) {
        self->cmd_set_night_window((uint8_t)s, (uint8_t)e);
      } else {
        reply = "{\"ok\":false,\"error\":\"start/end must be 0..23\"}";
      }
    } else if (action == "override_minutes") {
      long n = 0;
      if (json_get_number(body, "value", &n)) {
        self->cmd_set_override_minutes((uint16_t)n);
      } else {
        reply = "{\"ok\":false,\"error\":\"missing value\"}";
      }
    } else if (action == "exit_manual") {
      self->cmd_exit_manual();
    } else if (action == "night_caps") {
      long q = -1, l = -1, a = -1, lo = -1;
      json_get_number(body, "quiet", &q);
      json_get_number(body, "light", &l);
      json_get_number(body, "active", &a);
      json_get_number(body, "lost", &lo);
      if (q >= 0 && l >= 0 && a >= 0 && lo >= 0) {
        self->cmd_set_night_caps((uint8_t)q, (uint8_t)l, (uint8_t)a, (uint8_t)lo);
      } else {
        reply = "{\"ok\":false,\"error\":\"need quiet/light/active/lost\"}";
      }
    } else if (action == "night_throttle_ms") {
      long n = 0;
      if (json_get_number(body, "value", &n)) {
        self->cmd_set_night_throttle_ms((uint32_t)n);
      } else {
        reply = "{\"ok\":false,\"error\":\"missing value\"}";
      }
    } else if (action == "lost_timeout_ms") {
      long n = 0;
      if (json_get_number(body, "value", &n)) {
        self->cmd_set_lost_timeout_ms((uint32_t)n);
      } else {
        reply = "{\"ok\":false,\"error\":\"missing value\"}";
      }
    } else {
      reply = "{\"ok\":false,\"error\":\"unknown pet action\"}";
    }
  }

  if (reply == "{\"ok\":true") {
    /* 把最新状态一并回给前端，省一次往返 */
    reply += ",\"state\":";
    reply += self->json_state();
    reply += ",\"devices\":";
    reply += self->json_devices();
    reply += "}";
  }
  return send_text_(req, "application/json", reply);
}

/* ------------------------------------------------------------------ */
/* 其余命令与访问器（与网页强相关，放在本文件而不是主实现里）           */
/* ------------------------------------------------------------------ */

void PetStatusLamp::cmd_set_night_window(uint8_t start, uint8_t end) {
  if (start > 23u || end > 23u) {
    return;
  }
  pet_night_on_user_action(&this->night_, this->now_ms_());
  this->night_.cfg.start_hour = start;
  this->night_.cfg.end_hour = end;
  this->night_start_ = start;
  this->night_end_ = end;
  this->save_settings_();
  ESP_LOGI(TAG, "夜间时间窗 -> %02u:00-%02u:00", (unsigned)start, (unsigned)end);
}

void PetStatusLamp::cmd_set_night_caps(uint8_t quiet, uint8_t light, uint8_t active,
                                      uint8_t lost) {
  pet_night_on_user_action(&this->night_, this->now_ms_());
  this->night_.cfg.cap_quiet = quiet;
  this->night_.cfg.cap_light = light;
  this->night_.cfg.cap_active = active;
  this->night_.cfg.cap_lost = lost;
  this->cap_quiet_ = quiet;
  this->cap_light_ = light;
  this->cap_active_ = active;
  this->cap_lost_ = lost;
  this->save_settings_();
  ESP_LOGI(TAG, "夜间上限 -> Quiet %u / Light %u / Active %u / Lost %u",
           (unsigned)quiet, (unsigned)light, (unsigned)active, (unsigned)lost);
}

void PetStatusLamp::cmd_set_night_throttle_ms(uint32_t ms) {
  if (ms < NIGHT_ACTIVE_THROTTLE_MIN) {
    ms = NIGHT_ACTIVE_THROTTLE_MIN;
  }
  if (ms > NIGHT_ACTIVE_THROTTLE_MAX) {
    ms = NIGHT_ACTIVE_THROTTLE_MAX;
  }
  pet_night_on_user_action(&this->night_, this->now_ms_());
  this->night_throttle_ms_ = ms;
  this->night_.cfg.active_throttle_ms = ms;
  this->save_settings_();
  ESP_LOGI(TAG, "夜间 Active 节流 -> %u ms", (unsigned)ms);
}

void PetStatusLamp::cmd_set_lost_timeout_ms(uint32_t ms) {
  if (ms < 10000u) {
    ms = 10000u;
  }
  if (ms > 600000u) {
    ms = 600000u;
  }
  this->lost_timeout_ms_ = ms;
  this->save_settings_();
  ESP_LOGI(TAG, "失联超时 -> %u ms", (unsigned)ms);
}

/* ------------------------------------------------------------------ */
/* 服务器启动                                                          */
/* ------------------------------------------------------------------ */

bool PetStatusLamp::start_web_() {
  httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
  cfg.server_port = this->web_port_;
  cfg.lru_purge_enable = true;
  cfg.max_uri_handlers = 8;
  cfg.uri_match_fn = httpd_uri_match_wildcard;
  /* 栈留够：JSON 拼接与字符串比较都在处理器里做 */
  cfg.stack_size = 8192;

  if (httpd_start(&this->httpd_, &cfg) != ESP_OK) {
    this->httpd_ = nullptr;
    return false;
  }

  static const httpd_uri_t routes[] = {
      {"/", HTTPD_GET, pet_http_index_, nullptr},
      {"/index.html", HTTPD_GET, pet_http_index_, nullptr},
      {"/api/pet/state", HTTPD_GET, pet_http_state_, nullptr},
      {"/api/pet/devices", HTTPD_GET, pet_http_devices_, nullptr},
      {"/api/pet/settings", HTTPD_GET, pet_http_settings_, nullptr},
      {"/api/pet", HTTPD_POST, pet_http_post_, nullptr},
      {"/api/light", HTTPD_POST, pet_http_post_, nullptr},
  };

  for (const auto &r : routes) {
    httpd_uri_t uri = r;
    uri.user_ctx = this;
    if (httpd_register_uri_handler(this->httpd_, &uri) != ESP_OK) {
      ESP_LOGW(TAG, "注册路由 %s 失败", r.uri);
    }
  }

  ESP_LOGI(TAG, "内置网页已启动：http://<设备IP>:%u/  （Basic Auth 用户 %s）",
           (unsigned)this->web_port_, this->web_user_.c_str());
  return true;
}

}  // namespace esphome::pet_status_lamp
