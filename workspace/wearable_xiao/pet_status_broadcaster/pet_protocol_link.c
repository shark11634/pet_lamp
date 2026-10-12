/*
 * pet_protocol_link.c — 把 core/ 的协议实现拉进 Arduino 构建
 *
 * 为什么需要这个文件：
 *   Arduino / arduino-cli 只编译 sketch 目录内的源文件，
 *   而协议实现只有一份，位于 workspace/core/pet_protocol.c。
 *
 *   直接复制会造成"两份实现漂移"，因此这里**不复制代码**，
 *   只用一行 include 让构建系统编译到那份唯一实现。
 *
 *   头文件搜索路径由编译选项提供：
 *     arduino-cli compile --build-property "compiler.c.extra_flags=-I..\..\core" \
 *                         --build-property "compiler.cpp.extra_flags=-I..\..\core" .
 *   或在 Arduino IDE 的"更多编译器选项"里加同样的 -I。
 *
 * 该文件同时是把佩戴端挂到 core/ 的唯一入口，
 * 因此 core/ 里新增模块时，只需在这里追加对应的 include。
 */

#include "../../core/pet_protocol.c"

/* 若要启用设备表/夜间模式（佩戴端目前不需要），在这里追加：
 *   #include "../../core/device_registry.c"
 *   #include "../../core/night_mode.c"
 * 佩戴端只需要协议层，因此默认不引入灯端逻辑。
 */
