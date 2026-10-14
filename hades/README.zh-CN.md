# Hades 目标：真机图形里程碑，完整移植未完成

English: [README.md](README.md)

此目标**尚未完成完整可玩的 iOS 移植**。第 22 版已取得真机图形里程碑：
用户反馈成功，目录顺序调整后的两个绘制窗口均无零绘制通道。
完整试玩稳定性仍待验证，后台挂起仍未实现。已验证本机解包、四个常驻模块及 EOS NRO 的
平台 ABI 改写、SPIR-V 到 MSL 翻译、iOS ARM64 嵌入对象，以及 macOS 原生
AOT 模块/计时门与两份真实着色器的离屏 GPU 读回。现已链接独立 iPhoneOS ARM64
验证 App，并另附完整 RomFS 本机资源包；打包器产物只有 ad-hoc 签名。
用户本机配置 Apple Personal Team 后，第 10 版已完成开发签名、安装与真实设备启动。
显式宿主接管配置已在本机完成 SDK/驻留模块初始化并进入真实 `nnMain`，
现在已读取真实资源并提交存档，通过原生 socket/HID 初始化，创建 112 个着色器模块，
执行实际 indexed draw、队列提交及呈现，并通过原生 SDK 循环提交与回收 PCM 音频缓冲。
本进程 swapchain GPU 读回与受控键盘已验证真实主菜单→选项→返回。
另一路独立新存档运行 125 秒、14546 次呈现，固定 1280×720 下 SUBOPTIMAL 为零；
后续场景不等同经过核对的移动或战斗。未配置输入时仍停在 SelfController 命令 9，
通用库小程序后端仍未实现。真机启动及第 22 版用户反馈的画面成功见下，
完整试玩仍待核验。
`target.json` 定义本体应用 `0100535012974000`、
内容版本 0；构建 ID、哈希、导入及生成的游戏内容保留在私有本机收据中，
不作为地址表进入版本控制。

## iPhone 15 Pro Max 实测检查点（2026-10-14）

第 10 版已在用户连接并授权的 iPhone 15 Pro Max / A17 Pro / iOS 26.7.1
安装启动。采用 Personal Team 诊断配置，没有请求两个扩展内存权限；这只证明本次
实际运行到达的阶段，不保证其他设备/系统/内存压力下能运行。
构建、设备及账号标识、完整日志均仅保存在私有本机收据，不写入版本控制。

| 阶段 | 实际证据 | 尚不能证明 |
|---|---|---|
| 原生核心 | 两次 `CORE_OK`、返回 0；200398 项重定位、EOS 调用、TSD/计时门 | 游戏线程整体并发正确 |
| 离屏 GPU | 72 份 MSL 编译、三次 `FRAME_OK`、`RENDER_OK`、返回 0 | 客体 Vulkan 游戏像素及可见呈现 |
| SDK | 六初始化入口、SDK `DT_INIT`、`HOST_BOOTSTRAP_RETURN`，返回 0 | 其他游戏构造器/完整玩法 |
| 实际游戏入口 | 3066 资源全部设备 SHA-256 校验、真实 `nnMain`、A17 Pro Vulkan 呈现至少 2400 次成功 | 屏幕内容、触控消费、移动/战斗正确 |
| 本地存储/音频 | `SAVE_COMMIT_OK`、真实音频服务打开 | 退出重进恢复存档、用户确实听到声音 |

第 2400 次呈现观测 resident 约 714 MB、physical footprint 约 1.77 GB、
Metal allocated 约 903 MB；这些账目重叠，不能求和，不是长期稳定性或 Jetsam 配额。
仍须在前台实测主菜单/选项、进入关卡、移动/攻击/闪避、声音，以及退出重开后的存档恢复；
没有这些设备证据不能称“真机可玩”。用户随后确认已进入场景并游玩数分钟，
但第 10 版发生下述崩溃，不能称稳定可玩。活动诊断后台终止限制仍保留。

## 第 11 版描述符分配修复

第 10 版真机记录至少 10200 次成功呈现后发生 SIGSEGV，匹配安装包符号和
实际 Mach-O 装载基址后，故障指令位于 MoltenVK
`mvkUpdateDescriptorSetWithTemplate`：对空 `VkDescriptorSet` 解引用。
此前多次 `vkAllocateDescriptorSets` 返回 `VK_ERROR_OUT_OF_POOL_MEMORY`。
系统 CPU 资源报告写明未采取终止措施；本次不是后台主动退出，也不能仅凭 footprint
归因于 Jetsam。手机存档已只通过 USB 备份到私有本机目录，没有删除或上传。

实际客体请求的批次包括 6144/6528 个集合，布局各需三个 uniform-buffer 描述符，
但相应原始池只提供 8192 个这类描述符。第 11 版按已记录布局逐项计算真实需求，
仅在实际池容量/碎片失败时创建独立驱动 backing pool，并分配真实集合；
保留客体 `maxSets` 上限、所属设备/分配器及 free/reset/destroy 生命周期。
不乘任意容量常数、不伪造成功；真正内存分配失败仍返回错误，
空集合更新会明确拒绝而非继续进入驱动。

O0/O2 实际 Metal 测试覆盖不足配额、模板更新、maxSets 耗尽、
重复释放拒绝及 free/reset/destroy。全部 72 项回归通过；
手机存档副本的可见 macOS 原生运行与受控输入到达至少 13200 次呈现，
未再记录描述符分配失败、SIGSEGV 或未实现服务。
这不等同修复后的真机稳定性验证：第 11 版已开发签名并通过 USB 更新，
设备锁屏阻止了自动启动，需解锁后再次前台点击第 4 项复测原场景。

## 第 12 版 iOS 纹理诊断

用户在第 11 版设备画面中确认部分前景/HUD 为灰色轮廓，而 Mac 纹理正常。
第 11 版后续记录至至少 10800 次成功呈现，未见描述符分配失败；
本次最后退出记录是前台限制，而非先前 SIGSEGV。
这仍不代表关卡渲染正确。

第 12 版保留原渲染行为，添加有界的实际图像格式、尺寸、mip/layer、
view 通道映射，以及 Metal/Vulkan 压缩纹理能力和 BC 格式查询日志。
Mac 实测包含 `VK_FORMAT_BC7_UNORM_BLOCK`（145）、BGRA8、RGBA8、R8，
view 的通道为 R/G/B/A，Vulkan BC 能力为真。
这些只说明 Mac 路径，不能外推 A17 Pro 的 iOS 驱动。
必须取得同版本设备的 `METAL_TEXTURE_CAPABILITY`、
`TEXTURE_COMPRESSION_FEATURES`、`TEXTURE_FORMAT_QUERY` 和
`TEXTURE_IMAGE_CREATED`/`TEXTURE_VIEW_CREATED` 后，才能确定缺纹理是否来自
压缩格式支持、资源选择或采样路径；当前没有把猜测性的颜色替换称为修复。
第 12 版已通过回归、开发签名和 USB 更新，但自动启动仍被设备锁屏拒绝，
需解锁并前台点击第 4 项获取设备对照证据。存档和 RomFS 不重新创建。

## 第 13 版绑定路径单变量对照

第 12 版设备对照已经取得：A17 Pro 的 Metal BC 能力、Vulkan BC/ETC2/ASTC
均为真，BC7（145）支持 sampled/linear-filter/transfer，实际图像格式和
R/G/B/A view 映射与 Mac 一致，无驱动分配错误。
因此不能把灰色轮廓直接归因于“不支持 BC7”。用户还报告画面闪动；
资源绑定与跨帧同步需继续区分，尚未确定根因。

第 13 版通过 `HadesVulkanBindingMode=direct` 明确启用单变量实验：
在首次 Vulkan 初始化前使用 MoltenVK 官方
`MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS=0`，改为直接资源索引。
`argument-buffers` 值可用于后续对照；缺失/非法配置或设置失败明确拒绝启动。
没有改材质、着色器、纹理数据、队列同步或伪造压缩能力。
Mac 实际日志确认 `discrete resource indexes`，90 秒受控场景运行至少
9000 次成功呈现，四次本进程 GPU 读回，没有描述符失败或 SIGSEGV；
72 项回归通过。第 13 版已 Apple 签名、USB 安装并启动，
但尚未点击真实游戏入口；颜色和闪动改善必须分别在原场景核验，
不能把此实验配置称为已验证的 iOS 修复。

## 第 14 版同步对照（用户确认无改善）

用户实测第 13 版仍缺颜色且闪动，设备日志确认确实使用直接资源索引；
因此该绑定切换没有解决症状。第 14 版恢复原 argument-buffer 配置，
仅通过显式 `HadesVulkanQueueCompletionProbe=true` 在每次成功
`vkQueueSubmit` 后执行真正 `vkQueueWaitIdle`。成功完成记录
`QUEUE_COMPLETION_PROBE_WAIT_OK`；失败返回真实驱动错误，不伪造完成。
这会串行化 GPU、改变时序和性能，仅用于区分资源复用/同步问题，不是最终方案。

72 项回归通过，O0/O2 真实 GPU 测试分别覆盖该探针开/关，
第 14 版已开发签名且 15 段嵌入及 293 份 HadesData 资源核验通过。
重新连接后确认无 Hades 进程，已通过 USB 安装并启动；
最新启动日志确认 `build=14` 和 argument-buffer 配置。
更新前后 10 份存档文件 SHA-256 完全一致，没有卸载 App。
用户已进入真实游戏，当前构建日志确认完整 RomFS 3066 文件核验、
至少 12000 次提交后的队列等待成功，以及至少 4800 次成功呈现；
该次观测无崩溃或驱动错误记录。用户确认颜色缺失与闪动均仍存在，
因此提交后等待队列完成没有解决症状，不应继续作为修复方案。
这并不排除单次提交内部的通道可见性、附件写回或 CPU 上传问题；
这些计数不证明画面正确，也不证明跨帧同步是根因。
尝试的底层上传首块读回未能取得有效源缓冲，已撤下此探针，
没有把它记录为纹理像素正确性证据。缺颜色和闪动的根因仍未确认。

## 第 15 版像素诊断（已取得真机场景，尚未修复）

参考 D2R 的纹理转储和附件/屏障证据方法，保留实际 Vulkan 路线，
不复制 NVN 地址、格式号或 Metal 4 专用回放补丁。
本版关闭已被用户实测否定的全队列完成探针，恢复 argument-buffer 基线，
显式启用 `HadesVulkanPixelCaptureProbe=true`。仅用户选择真实游戏入口后，
在 App 自己的 Documents 下创建独立 `HadesRenderProbe-<UUID>` 目录。
每 600 次呈现保存一次，最多四次；只读本 App 的 GPU 资源，不抓桌面或其他 App。

诊断保存最终交换链 RGB 图，以及最多 512 个存活图像的取样清单和
64×64 以内的 RGB 缩略图。使用实际 GPU 读取 R8/RG8/RGBA/BGRA 和 BC7，
包括 2D 数组的第 0 层；sRGB 经实际 GPU 读取转换。
清单记录图像序号、句柄、格式、尺寸、用途和捕获状态。
不支持的格式、不可获取的底层纹理、表容量溢出均明确记录，不冒充像素验证。
文件以 0600 权限独占创建，不覆盖旧证据；销毁图像同步撤销诊断登记。
另有界记录前 32 个 render pass 的 load/store/layout/dependency，
及前 64 次 pipeline barrier 的阶段、访问和图像转换；不修改这些客体命令。

这会增加 GPU 工作和等待、扰动时序；不是生产修复。
缩略图只对应**帧末底层存储的第 0 mip/第 0 层**，不是每个通道结束时的快照，
不包含 alpha，也不能证明客体实际使用的 view、sampler、descriptor 或片元程序正确。
仅凭其最终颜色不能归因于某一缺失物件。

O0/O2、队列完成开/关四组真实 GPU 回归覆盖 Vulkan 上传后的
R8/RGBA/BGRA/BC7 精确已知 RGB、2D 数组首层、同一图像重新上传后像素改变、
图像销毁后清单清空、重复文件创建拒绝及通道/屏障日志字段。
全部 72 项回归通过。Mac 真实游戏从独立存档副本运行约 45 秒，
记录至少 4800 次成功呈现、四组捕获和 1340 份 PPM：
后三组各登记 406 个图像，338 个成功取样、6 个格式/用途不支持、
62 个底层纹理不可取或类型不支持，登记表无溢出；没有记录原生崩溃或驱动失败。
这些未捕获图像仍是调查缺口，不能声称已覆盖全部绘制。

最终 iPhoneOS 包已 Apple 签名，15 段嵌入与 293 份资源及冻结源码哈希已核验。
恢复连接后确认没有 Hades 进程，已通过 USB 安装并启动最终本版，
启动日志确认 build=15 和对应冻结构建收据。
更新前后 10 份存档文件 SHA-256 完全一致，没有卸载 App 或中断游戏。
用户进入游戏后，已通过 USB 取得四张最终帧和 1662 张纹理缩略图，
共 1666 份 PPM。后三组各成功取样 444 个图像，
另有 6 个不支持、62 个底层纹理不可取；登记溢出计数达到 41，覆盖不完整。
最终帧中可直接观察到大块黑色剪影。

另从更新前存档的独立副本运行一次有界 Mac 对照，实际进入大厅场景，
不再以菜单作为场景基线。iOS 与这次 Mac 捕获各有 279 种不同的 BC7 RGB
缩略图，全部哈希一一相同。此证据只说明已观察的第 0 mip/第 0 层 RGB
存储一致，不能证明 alpha、其他 mip/层、实际绘制采样或附件合成正确。
R8 动画平面和渲染目标受不同动画时刻、镜头影响，不能仅凭哈希不同判定损坏。
此次 Mac 场景同样超过 512 项登记容量，溢出计数达到 32。
灰色素材和闪动尚未修复；仍需补齐登记覆盖与 alpha/view/descriptor/逐通道证据。

### 第 16 版透明度与登记诊断

第 16 版将存活图像登记上限提高到 2048，并从每张 RGB 缩略图的同一次 GPU
读回额外保存 `image-<serial>-frame-<frame>.ppm.alpha.pgm` 灰度透明度图。
PGM 同样以 0600 权限独占创建；透明度文件缺失或写入失败会使捕获失败，
不报告只有 RGB 的成功。第 0 mip/第 0 层及最近邻缩小的限制仍然存在。
启用捕获时，图像/view 创建日志覆盖最多 2048 项，新增 view 句柄，
便于关联来源图像，而不假设 Mac 与 iOS 创建顺序相同。

真实 GPU 回归使用非全不透明的 RGBA/BGRA，验证同一图像重新上传后 alpha 改变；
另创建 600 个存活图像，超过旧版 512 上限，并验证销毁后登记全部撤销。
这是诊断覆盖增强，不是渲染修复；全部 72 项回归通过。
有界 Mac 大厅运行取得 1770 份 RGB PPM 和 1766 份 alpha PGM；
后三组各登记 544 个存活图像，476 个成功取样、6 个不支持、62 个底层不可取，
四组登记溢出均为零。

最终 v16 Apple 签名及 15 个嵌入段已核验。确认手机没有 Hades 进程后，
已通过 USB 更新并启动宿主；更新前后 10 份存档哈希一致，
启动日志确认 build 16 和冻结收据，没有中断游戏。
用户点击第 4 项后，已通过 USB 取得 1770 份 RGB PPM 和 1766 份 alpha PGM。
四组登记溢出均为零；最后一组登记 568 个图像，500 个成功取样、
6 个不支持、62 个底层不可取。279 种不同 BC7 的 RGB 加 alpha 缩略图组合
全部与 Mac 真实场景像素一致。已观察到的 BC7 view 也均为相同格式、
恒等通道映射及单个基础 mip/层；尚不能证明 descriptor/绘制或逐通道输出正确。

### 第 17 版附件加载对照

两端场景日志的前 32 个 render pass 颜色附件均为 `DONT_CARE` 加载、
`STORE` 写回，初始和最终均为颜色附件 layout，且无通道 dependency。
仅凭这些声明不能确认错误。第 17 版显式启用
`HadesVulkanAttachmentLoadProbe`，仅在临时复制给真实驱动的描述中，
将颜色附件到颜色附件 layout 的 `DONT_CARE` 改为 `LOAD`。
原客体结构、CLEAR/LOAD、其他 layout、深度、模板、store 和 dependency 均不改动。
运行时默认关闭，仅 `HADES_VULKAN_ATTACHMENT_LOAD_PROBE=1` 才启用；
iOS 要求显式布尔配置，实际改动会有界记录。

O0/O2、加载对照与队列完成对照的开/关组合验证真实 GPU 跨附件通道保留
RGB 与 alpha、精确选择条件以及调用方结构不变。
这是分块 GPU 丢弃旧附件内容的假设对照，不是已确认修复，
也不声称 Vulkan 未定义的附件内容具有跨平台保证；仍需真机对照和视觉确认。

用户自行退出游戏后已安装签名 v17，10 份存档哈希一致。
设备日志确认 32 次真实加载改动及四组成功、无溢出的纹理捕获；
用户确认**颜色和闪动均没有改善**。该对照为阴性，不是修复；
下一版关闭此开关。

### 第 18 版逐通道像素

显式 `HadesVulkanPassCaptureProbe` 选择第 1800 次呈现计数窗口；
进入该次呈现调用时关闭新通道选择，不保证覆盖客体多线程的完整逻辑帧。
每个选中 render pass 结束后，在同一客体 Vulkan 命令流内记录图像到独立
缓冲区的复制，保留后续通道覆盖前的内容；随后恢复声明的 final layout。
实际提交队列完成后，从 coherent 内存保存完整分辨率 RGB 及 alpha。
日志关联命令、framebuffer、实际 begin 使用的 render pass、图像、
附件、绘制次数和快照序号。仅取证开启时另关联着色器字节 FNV-64 指纹、
管线阶段、附件混合/写掩码、通道开始、管线绑定和索引绘制参数。
指纹只作本机关联，不是密码学核验。
管线创建元数据限 2048 项、选中绑定限 1024 项、索引绘制限 2048 项；
达到这些上限明确记入日志。

最多观察 96 个单层、单采样 RGBA8/BGRA8 UNORM 颜色附件；
每份读回最多 16 MiB、未完成像素缓冲总计最多 384 MiB。
深度、其他格式/采样和多层输出明确跳过。
取证需要额外的 TRANSFER_SRC 用途及复制屏障/队列等待，会扰动选中帧，
因此仅捕获期间问题消失也不能直接视为生产修复。
已记录复制尚未提交的命令若重录，会明确拒绝，而非输出过期快照；
登记、分配和写入失败均显式报告。

真实 GPU 回归覆盖 O0/O2 及三个诊断开关的开/关组合：
已知附件在通道结束时复制，随后在同一命令缓冲内覆盖；
逐通道图保留之前的 RGB/alpha，帧末图则显示新像素。
全部 72 项回归通过，证明不是另一份帧末取样；
窗口回归另确认呈现开始后，即使成功呈现计数尚未更新也不再选择新通道。
实际 Mac 大厅已从一个命令缓冲写出 74 组完整分辨率 RGB/alpha，
不再被原来 64 份上限截断。早期黑色附件之后逐步形成有颜色的场景；
不能单凭黑色中间通道判定 iOS 缺陷。最终关联版 Mac 写出 73 组，
所用 16 种管线均有元数据关联，未触及管线或像素上限。

签名 v18 已通过 USB 安装，10 份存档哈希不变，启动核对 receipt 一致。
真机前台实际场景写出 72 组逐通道 RGB/alpha，无截断或读回错误，
最终画面仍有黑色剪影。所跟踪命令流有 394 次索引绘制、36 个零索引绘制
通道；Mac 对照有 545 次、1 个。动画和绘制排序不同，不能盲按序号配对；
这些计数是观察结果，尚未证明 CPU 或深度缓冲就是根因。

### 第 19 版调用流对照

同一显式开关额外跟踪第 1800 和 3600 次呈现计数窗口的原始 Vulkan 调用。
只有第一个窗口插入逐通道复制，第二个不插入此类复制/屏障/等待；
默认 iOS 捕获步长的四次帧末清单也在第二个窗口前结束。
这将较晚的调用流观测与读回帧干预分开，并不消除 CPU 日志的时序影响。

最多跟踪 16384 次调用，记录参数、客体 LR/FPCR、render-area/scissor 声明，
以及通道结束时的 callee-saved CPU 寄存器；本机模块映射可用于去除 ASLR 后
定位调用点。不转储客体栈、资产或缓冲区内容。
已有私有捕获配置下用 `HADES_VULKAN_PASS_CAPTURE_FRAME=1800`，
或 iOS 的 `HadesVulkanPassCaptureProbe` 开关启用。
这仍是诊断，不是修复。

签名 v19 已经 USB 安装，10 份存档哈希不变，启动 receipt 一致。
真机至少完成 6000 次成功呈现；两窗口原始索引绘制分别为 392/403 次，
没有调用截断。较晚窗口没有逐通道复制，共 73 个通道，其中 34 个无索引绘制；
Mac 较晚窗口为 557 次绘制、76 个通道、1 个无索引绘制通道。
两端 begin/bind/draw/end 调用点去除 ASLR 后相同，FPCR 均为零。
这证实所观测场景的提交差异，不代表精确同帧对照，也没有确认差异根因。
尚未修复缺色和闪动。

### 第 20 版标量状态观测

同一显式开关在管线绑定时采样邻近静态标量加载目标。
仅在已登记可读可执行的客体代码内，最多检查 576 字节，解码相邻
`ADRP`/无符号立即数整数 `LDR`；只读取已登记、可读的模块数据中的
1/2/4/8 字节值。每次最多八项，超出明确记录，不新增固定游戏地址、
指令补丁、客体栈/资产内容转储或 GPU 操作。
日志是邻近指令加载目标的快照，**不证明该指令已实际执行**。
用于区分分批状态为空和后续绘制被拒绝。
仍用已有私有捕获配置及 `HADES_VULKAN_PASS_CAPTURE_FRAME=1800`，
或 iOS 的 `HadesVulkanPassCaptureProbe` 开启。

签名 v20 已安装并确认 receipt，10 份存档哈希不变。
真机至少完成 7200 次成功呈现，两窗口均取得标量观察，没有截断。
较晚窗口 75 个通道中有 31 个没有索引绘制，但其待处理批次数非零，
包括 16、34、30；Mac 较晚窗口实际提交 544 次索引绘制。
这排除了这些已观察通道的分批列表为空，并未确认具体拒绝条件，
也没有证明所有画面缺陷都由同一原因造成。

第 21 版在顶点缓冲绑定时再采样四个 Forge 着色器记录标量。
读取前必须匹配实际相邻指令指纹，且代码及模块数据范围已登记、可读；
不使用固定游戏地址，也不修改记录。描述符索引、每片描述符容量、
顶点步长和布局仍是观察值，不证明容量或纹理查找拒绝已经发生。
签名 v21 已通过现有 72 项回归及冻结映像/资源核验，
实际 Mac 场景两窗口取得 82/74 项字段观察，较晚窗口 554 次索引绘制，
没有调用截断。v21 随后经用户授权安装，启动 receipt 已确认，10 份存档哈希不变。
真机取得两个完整窗口的 143 项字段观察，至少 4200 次成功呈现；
较晚窗口 77 个通道、480 次索引绘制、25 个零绘制通道。
同一主场景着色器在 iOS 使用布局 2、Mac 使用布局 0，步长均为 56 字节。
真实客体容量表对布局 2 限制为 200000 字节，该窗口结束顶点计数停在 3571，
只余 24 字节，不能容纳一个新顶点。现有代码按布局注册编号分配并检查容量，
布局编号由着色器首次加载顺序决定；宿主目录枚举此前直接使用 `readdir` 顺序。
这定位了已观察布局的容量瓶颈，尚未证明目录顺序就是全部图形故障的根因。
不得把 Mac 数值替换为 iOS 能力限制，也不能把 Mac 运行结果当作 iOS 修复。

### 第 22 版只读目录顺序对照

`HADES_FS_DIRECTORY_ORDER_PROBE=1` 使只读资源目录快照按文件名字节序排序，
在过滤后、分页读取前完成；不改存档目录、客体代码、布局编号或容量表。
默认未设置或 `0` 保持旧行为，其他值明确拒绝。iOS 包由严格布尔配置
`HadesDirectoryOrderProbe` 开启该实验，启动日志明确标记，不能当作已验证修复。
目录回归覆盖开/关、大小写字节序、分页、文件/目录过滤、元数据及非法配置。
现有 72 项回归通过，Mac 实际场景至少 9600 次成功呈现，较晚窗口 555 次
索引绘制；主场景布局为 0。签名包已核对 15 个 AOT 段、291 资源及 42 源文件，
已获用户授权覆盖安装真机，启动 receipt 已确认，10 份存档哈希完全不变。
实际真机日志确认排序生效：主场景着色器 0/32/51 使用布局 0，步长仍为 56 字节。
两个窗口分别为 64/83 个通道、467/661 次索引绘制，零绘制通道均为 0。
较晚窗口主场景顶点计数达到 4641，超过此前 3571 上限，使用布局 0 的实际 4 MB 分配；
日志至少记录 3600 次成功呈现。用户反馈“成功了”，可记为真机图形里程碑，
不等于逐像素、逐症状或完整试玩验证。两次场景不同，汇总数不是逐帧对齐的性能比较。
原始 Horizon 目录顺序仍未核实。日志末尾另有既有的切后台主动终止记录，
后台挂起仍未实现。日志、签名产物核验和用户反馈均保留在本机私有产物目录。

第 9 版启动日志包含 `build` 和 `receipt`：由源码/头文件、五个改写映像、
绑定目录与 pinned 图形框架哈希生成，独立于输出目录；用于确认真机运行的是哪份待测构建，
不是 Apple 签名、正确画面或可玩证据。
参考 D2R 的自进程 Mach 内存观测，现记录 resident、resident peak、
physical footprint 和 virtual size；Vulkan 第一帧及每 600 次呈现另记录实际
Metal `currentAllocatedSize`。启动、诊断开始/返回及 UIKit 内存警告也记录内存，
不可获取的 Metal 数值明确标为 `unobserved`。这些是相互重叠的账目，**不能求和**，
2 GiB 虚拟堆也不等于物理占用或设备 Jetsam 配额。
O0/O2 测试真实触页 32 MiB，检查 resident/footprint 至少增长 24 MiB；
没有把本机数据外推为 iPhone 15 Pro Max 的内存可用性。

第 10 版支持由本机 CoreDevice 显式传入 `--hades-probe core` 或
`--hades-probe gpu`，在诊断视图出现且 App 为活动前台后执行一次对应测试。
普通启动不自动测试；重复/缺少/不支持的模式明确拒绝，不能自动执行 SDK 或游戏。
必须解锁手机并让 App 位于前台，启动命令返回成功不等同探针已开始；
以当前构建日志中的 `EXPLICIT_PROBE`、`TEST_BEGIN` 和最终 `CORE_OK` /
`RENDER_OK` 及返回码为准。不要将锁屏时存在的宿主进程当成原生游戏运行。

禁止将 Hades 文件交给 D2R 的 `setup.sh`、`product.sh`、宿主或 iOS 工程；
它们依赖 D2R 构建、地址、资源和界面。共享改写器新增明确的
`--profile portable-abi`：即使构建 ID 匹配 D2R，也禁用**全部** D2R 剪口、
观测、参数、私有规则和启动块清单。D2R 仍为默认配置，其专用表和规则未修改。
这是平台适配，不是完整游戏等价证明。

## 复现本机准备链

在 **git 之外**按 `toolchain.json` 指定的上游源码与版本构建 NSTool、
SPIRV-Cross。NSTool 按其上游要求初始化声明的子模块，运行 `make deps`，
再运行 `make`。SPIRV-Cross 使用清单中的 CMake 参数，再执行
`cmake --build <构建目录> --target spirv-cross`。不需要全局安装。

在本仓库根目录运行：

```sh
python3 -m venv "$HOME/hades-tools/venv"
"$HOME/hades-tools/venv/bin/python3" -m pip install -r hades/requirements.txt
PY="$HOME/hades-tools/venv/bin/python3"
WORK="$HOME/hades-work"
NSP="/absolute/path/to/Hades.nsp"
KEYS="/absolute/path/to/your/prod.keys"
NSTOOL="/absolute/path/to/nstool"
CROSS="/absolute/path/to/spirv-cross"
LLVM="/Applications/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin"

"$PY" hades/tools/prepare.py --work "$WORK" extract \
  --package "$NSP" --keys "$KEYS" --nstool "$NSTOOL"
"$PY" hades/tools/prepare.py --work "$WORK" rewrite --llvm-bin "$LLVM" --jobs 2
"$PY" hades/tools/prepare.py --work "$WORK" rewrite-dynamic --llvm-bin "$LLVM" --jobs 2
"$PY" hades/tools/prepare.py --work "$WORK" embed
"$PY" hades/tools/translate_shaders.py \
  --source "$WORK/data/RomFS/Content/EffectsForgeSwitch" \
  --out "$WORK/build/shaders-msl" --spirv-cross "$CROSS"
"$PY" hades/tools/probe_runtime.py --work "$WORK" \
  --out "$WORK/build/runtime-core" --spirv-cross "$CROSS"
"$PY" -m unittest discover -s hades/tests -v
```

解包必须使用**全新**工作区，不覆盖已有游戏文件。`prod.keys` 始终从提供的
绝对路径读取；NSTool 可读取同目录已有的 `title.keys`，或使用 NSP 内匹配的票据。
不将密钥值放入命令行，不输出或保存密钥内容。
验证全部 CNMT 内容大小和 SHA-256、NACP 标题、NPDM 程序 ID 及 NSO 段哈希；
解包前后核对原始 NSP 哈希。提供的包内票据被工具报告为假签名：
解密和内容哈希成功**不**能证明票据真实性或授权。

改写要求 `build/abi` 尚不存在。只有全部模块通过独立 LLVM 结构检查和
随机 Unicorn 等价验证，且每个非门站点至少有两次有效试验，才生成
`rewrite.json`。D2R 专用输出表必须为空。
未运行 D2R 原游戏回归：本次未使用 D2R 游戏文件。
iOS 嵌入入口拒绝验证后发生变化的原程序、改写字节或清单，并逐段比较对象文件
的字节和 16K 布局。**对象文件没有签名。**

`rewrite-dynamic` 从原始 EOS NRO 和包内 `.nrr/game.nrr` 开始，核对程序 ID、
哈希成员关系与 NRR 载荷的 RSA-PSS/SHA-256 签名，再无损规范化为共享改写器
接受的 NSO 容器。加载映像、BSS、动态符号和重定位逐项保持不变；原始 NRO
从不修改。证书内公钥的载荷签名验证**不是 Nintendo 根证书链认证**。
后续装载只接受此本机已核对包中被冻结的 NRR/NRO 哈希，不接受任意自签模块。
改写器变化后必须重验全部五模块；保留旧目录作为诊断，不可复用旧成功收据。

着色器入口检查每条 SPIR-V 指令边界和入口阶段，生成各阶段 iOS MSL 2.1
及绑定反射，记录工具、输入与输出哈希。指定**新输出目录**并加 `--compile`
才调用 Xcode Metal 编译器生成 metallib。缺少 Metal Toolchain 时明确报错，
不会用空库冒充成功。翻译或编译都不能证明运行期资源绑定和渲染正确。

`inspect_target.py check` 仍是只依赖标准库的只读环境和容器检查，
可用 `--nstool`、`--spirv-cross` 指定隔离工具路径；退出码 1 表示阻塞，
2 表示输入错误。`nso --exefs <路径>` 只检查头；`analyze_modules.py
--exefs <路径> --romfs <路径>` 进一步验证 NSO 哈希并生成静态导入、导出、
重定位、SVC 和动态 NRO 清单。清单入口退出码 0 **不**表示运行兼容。

## 已验证证据与剩余边界

实际包内元数据确认 **Hades / SupergiantGames，显示版本 1.0.34820**。
ExeFS 包含 `rtld`、`main`、`sdk`、`subsdk0`、`subsdk1`；后四个已完成
零 D2R 专用补丁的平台改写。27,519 个非门站点均有至少两次有效试验，
400 个负对照变异全部被检出。93 个 SVC/计时器门均通过结构验证；
本轮另实际执行 SDK 计时门及已实现的 GetInfo 熵查询，不代表全部门已具备宿主语义。
现有等价框架将独占操作归一化，因此不能证明实际并发原子操作、HLE、
真机 TSD 布局或完整游戏逻辑。

RomFS 另有 `nro/EOSSDK-Switch-Shipping.nro`，它提供 EOS 导入。
此 NRO 已完成 portable 改写和静态嵌入。签名页以 `vm_remap` 恢复原始布局，
没有 CPU JIT、RWX 代码页或运行期代码生成。新装载核心严格解析
RELATIVE/ABS64/GLOB_DAT/JUMP_SLOT，处理弱导入和符号可见性；未满足强导入、
未知重定位、代码页写入或越界都会失败。五模块共 **200,398 条**重定位已在
本机原生探针中实际写入。

`hades/runtime/core.c` 实现的是**解码后的 `ldr:ro` 命令核心**：进程注册、
NRR 注册、已嵌入 EOS 的授权映射、卸载和注销。拒绝 PID 不符、篡改哈希、
BSS 大小不符和错误生命周期；不是已接入客体 HIPC/SVC 的完整服务。
原生调用 `EOS_GetVersion` 返回 **1.7.1-13992660**；实际 SDK 计时入口经宿主门
返回单调的 **19.2 MHz** 时钟，8 个 pthread 各执行 100 次读取。
这只验证对应的宿主 TSD/计时路径，不证明客体线程、独占指令或完整并发正确性。

SDK 启动现已**原生完成并返回 `_init_libc0`、`nnosInitialize`、`_init_libc1`**。
新增内存层提供真实地址空间/alias/stack 查询、模块权限及当前 pthread 完整栈的查询，
按实际 NPDM 的 39 位地址空间界定范围；未知间隙标为不可访问，不伪造代码页。
主线程优先级 44、逻辑核心 0 来自 NPDM，不代表客体调度或宿主 CPU 亲和性已验证。
其 kernel capability 的允许优先级 28–59、逻辑核心 0–2 用于生成查询掩码，
而不是沿用 D2R 的掩码。

探针明确采用 256 MiB alias、128 MiB stack、2048 MiB 堆容量的宿主策略，
同一游戏映像在 512/1024 MiB 下出现空分配，而 2048 MiB 推进到音频和手柄支持初始化。
D2R 同样通过正确核算应用池避免分配失败，但不照搬其 3285 MiB 游戏特定常量。
用 `mmap(PROT_NONE)` 预留并按 2 MiB 对齐；这**不是实测 Hades/Switch 内存预算**。
`SetHeapSize` 真实开放页面，缩小后以不可访问的匿名映射替换已移除的尾部，
确保再次增长时清零。测试覆盖读写、缩小后的访问故障、前缀数据保留、重新增长清零，
以及非法尺寸和只读输出拒绝。这些堆路径是独立测试，不是本次 SDK 启动已观测的调用。
堆容量、开放堆和映像大小是模型账目，不是物理 RSS。
现已实现宿主页粒度兼容的 `MapMemory`/`UnmapMemory`：真实共享别名、禁止访问被借用的
源页、解除映射后恢复读写，并拒绝缩小/卸载被借用的内存。权限/属性修改及客体调度仍未完成。

`ConnectToNamedPort("sm:")` 与 SDK 实际 `SendSyncRequest` 已接入有生命周期、
不复用旧编号的句柄及当前线程 256 字节 HIPC 缓冲区。协议子集校验 CMIF v0/v1、
内核绑定的 PID、当前进程 copy handle、会话控制、域对象身份/关闭及 copy/move 响应。
未知命令和描述符明确失败。实际启动已完成 `sm:` 注册，取得 `lm`、`apm`、
`appletOE`、`aoc:u`，转换两个 domain 并创建 SDK 请求的对象。
这些是**部分服务端点**，不是完整系统服务：日志提交、性能配置 setter 及未观测的
applet 方法仍未实现。APM normal/handheld 是显式虚拟策略，不是实测设备状态；
applet resource ID 绑定到已认证的内核进程。

暂停计时/GPU 错误 reader handle 有真实等待、清除和单调时钟超时状态，
宿主通知 API 已用 pthread 等待者验证；第 8 版接入 UIKit 前台失焦/恢复，
但后台客体线程暂停、时钟冻结和 Metal 在途提交排空仍未实现；
不伪造暂停或 GPU 错误通知来推进初始化。离屏启动明确采用单应用虚拟前台策略，
发布一次初始焦点消息，记录前台授权和焦点处理策略；若策略要求暂停客体线程，
宿主暂停转换会明确失败，不冒充已接入 iOS 生命周期。AOC 仅报告当前挂载的基础包集合
（零 DLC）并提供真实未触发的内容变化事件，不继承 D2R 的 DLC 身份，不支持动态安装。

从 `sdk+0x11f4000` 借用 **4 KiB** 的请求现已在 **16 KiB** 宿主页上通过：
Hades 探针显式启用 D2R 式**复制搬移兼容策略**，只扩大开放自有 stack 预留页，
复制请求字节，逻辑上撤销源权限，但不物理保护源页。归还时复制回源并清零目标请求范围，
不收回共享宿主页权限以免伤及相邻借用。这依赖客体借用期间不访问源，**不是实时共享别名，
也不是物理 4 KiB 权限隔离**。精确宿主页请求仍用原共享别名路径，同一目标宿主页禁止混用
精确别名与复制映射。独立匿名页实验确认，`vm_remap` 返回成功也可能
不能满足指定的 4 KiB 偏移别名；保护 4 KiB 源区间会连带禁止同一宿主页内的相邻客体页。

新增 `threads.c` 参考 D2R 的 stopped pthread/启动/SDK 栈切换，以及修正后的
“先发布条件键、后释放 mutex”防丢唤醒顺序。实际 SDK 后台线程 `0x101` 已以优先级 28、
逻辑核心 2 在上述 4 KiB 借用栈上启动，并真实等待 GPU 错误事件。
锁交接及条件唤醒按优先级/FIFO 排序；单线程句柄等待支持单调时钟有限、零及无限超时。
在相关 pthread join 前禁止解除栈映射及注销客体内存；当前保守策略不允许两个 pthread
共用同一借用栈映射。优先级继承、地址仲裁、线程/事件混合等待、完整调度和全部独占指令语义
仍未验证或实现，不能称为完整客体并发正确。

Hades 专用 `HADES_HOST_GATE_STACK` 分支保留 800 字节客体寄存器帧，
但将宿主 C 调用移到每线程独立的 64 KiB 栈；未定义宏的 D2R 原路径不变。
合成 4 KiB 客体栈探针在 O0/O2 下运行 24 KiB C 帧，核对 GP/SIMD/NZCV/FPSR/FPCR
恢复及寄存器输出改写；八次 O0/O2 合成 pthread 探针各执行 64 次 mutex/condition 交接。
这些是有边界的宿主测试，不是游戏完整并发证明。HIPC/TLR 显式 16 字节对齐，
修复线程 TLS 布局变化后与 SDK 按绝对地址对齐 CMIF 不一致的问题。

原始纯 IPC 配置仍在 `pctl` 处停止，作为未接管基线保留。新的显式
`--host-services` 配置参考 D2R 按符号强制接管，不使用其地址：
只接管真实导出的零 addend 函数引用，指向预签名静态桩；
独立符号门同样使用 800 字节寄存器帧和 64 KiB 宿主栈。
**六个 SDK 入口和四个驻留 DT_INIT 已全部返回，三次本机原生运行进入真实 `nnMain`**；
DT_INIT 自身处理 init array，不重复调用。

已实际执行宿主时钟/时区初始化、128 MiB 客体自有只读 RomFS 窗口缓存、
持久离线本地用户打开和真实存档目录创建。文件操作使用真实 owned descriptor，
时间/日历使用宿主时钟；APM 保存虚拟配置，不冒充改变 iOS CPU/GPU 频率。
本地用户不是 Switch/Nintendo 网络账号，联网认证仍未实现。
存档挂载、文件/目录操作、写入、同步与原子提交已实现。
真实 `working`/`pending`/`committed` 快照、独占锁、文件/目录 fsync 和原子交换，
在失败时保留上次提交；进程中断恢复测试不等于断电持久性证明。
ROM 挂载缓存大小改为实际使用的 4 KiB 元数据/读窗口；原先返回 0 会使游戏跳过
`Content:/` 挂载。现在真实游戏已读取资源、启动四个额外 SDK pthread 并提交 `activeProfile`。
原宿主配置停在 `nvdrv`。实验 Vulkan 配置所需的
**6,160,384 字节 CodeData 传输池**已具备真实实时共享别名，源页 permission=0 且物理不可访问。
显式使用 `rewrite --module-bias main:0x3000`，重新生成 portable 分支、
key/trampoline 几何及签名段零前缀，将真实源区间对齐 16 KiB，不改变原模块内部偏移。
四份 NSO 与 EOS 在新的私有产物区重新验证，不能复用旧改写收据或直接偏移旧改写映像。
BSD 后端持有真实传输池引用与本机监控 pipe，支持 domain 会话克隆，Internet socket 明确失败。
实际 SDK 版本 7 要求返回有符号注册状态，而版本 1 返回 PID；现已正确区分。
原生 socket 初始化及监控订阅已通过，没有照搬 D2R 的初始化成功空桩。

原生 HID 初始化也已通过：实际 256 KiB publisher 映射至已预留非堆空闲范围，
客体接收端为物理只读。保存 revision 3 激活、手柄样式/ID 与 Joy-Con 方向，
真实 256 字节接收缓冲区复制经过检查的 HIPC 指针输入；五个样式事件发布初始断开状态，
支持等待、清除与关闭。不声称有物理手柄；配置前端才连接下述虚拟 FullKey publisher，
未配置前端时仍保持断开。
共享 capability 重复获取、不支持的 domain/revision 及仍在映射的内存关闭明确失败。
O0/O2 测试验证 publisher 写入后的实时读回、接收端物理写故障及资源生命周期。
新增 O0/O2 符号门、文件缓存、时钟/日历、账户持久化及非法输入测试，
仍不等同完整客体并发正确性或真机可玩。
不能直接调用 DT_INIT 跳过 SDK/主线程初始化；
此前诊断已确认这会因缺少 ThreadLocalRegion 中的真实 SDK 线程对象而失败。

`subsdk1` 虽导出 EGL/OpenGL，但实际 main 导入 **100 个 Vulkan 函数**，
包含 `vkCreateViSurfaceNN`。之前从 SDK 导出推断游戏走 EGL/NVN 并不成立。
实际 Switch 资源包含 **53 个片元和 19 个顶点 SPIR-V 二进制**，
全部 72 个已翻译成 MSL 并生成反射，不选择桌面版资源。
D2R 的 Maxwell/CASC 提取规则不适用。新增 `reflect_bindings.py` 重跑反射、
核对生成 MSL 哈希并匹配真实 `buffer/texture/sampler` 参数槽。210 项声明中，
206 项进入 MSL 参数，另四项明确标记为未被该 MSL 接口发出，不以猜测槽号补齐。
SPIR-V 的 descriptor set/binding **不能直接作为 NVN 槽号**。

`dm/gfx/nvn/nvn_spirv_bind.c` 消费现有 NVN `OP_BIND_UBO/TEX/SAMP` 载荷，
按调用方显式提供的程序映射转交 Metal；必需绑定缺失、大小不足或槽号冲突会失败。
离屏探针以真实 `Default2D.vert` / `Default.frag`、显式测试客户端绑定与合成顶点/纹理，
全部 72 个阶段均通过 macOS 自带 `newLibraryWithSource` 编译；
这两份着色器另完成 GPU 提交及像素读回：
三帧分别有 **1024 / 440 / 1024** 个匹配像素，验证投影 uniform 与纹理变化生效。
未调用或安装 Xcode Metal Toolchain；这不是生成 iPhoneOS metallib。
上述 NVN/MSL 测试客户端不是真实游戏的 Vulkan 渲染路线。
可选 Vulkan 配置使用官方 public-API MoltenVK v1.4.2 ARM64 静态库，
核对归档 SHA-256 和解出的库、头文件、许可证字节，不修改全局工具链。
带类型的 AOT 转发正确处理超过八个寄存器参数的 Switch 栈 ABI；
NN VI surface 映射至真实 CAMetalLayer，宿主门栈扩大至 1 MiB。
O0/O2 测试验证十参数转发、真实 Metal 队列缓冲区复制/读回与映射撤销。
带类型的 allocation/debug 回调现使用每线程独立、带 guard 的可重入栈，并验证
真实 SDK Thread/TSD 身份；驱动自建线程及 instance pNext 回调仍明确拒绝。
分配回调 ABI 由合成 typed PFN 单独验证；该 MoltenVK 测试路径没有调用这些分配回调，
不能混同为真实驱动分配证据。驱动 debug 通知另有真实回调验证。
真实游戏已创建 112 个着色器模块并提交 draw/present；原样保留 `VK_SUBOPTIMAL_KHR`，
它与 `VK_SUCCESS` 均作为有效提交原子计数，并保留前端显式 drawable 尺寸。
这些提交仍不是正确可见像素或 iOS 试玩证据。现有本机探针可用 `--window` 附着
AppKit 窗口；它不替代真机。只针对该窗口的本机截图尝试失败，未绕过权限或改抓桌面。

原生 `audout:u` 现接入真实 48 kHz PCM16 AudioQueue，复制经过可读性验证的 PCM，
以不透明 tag 回传播放完成缓冲。空释放队列必须清零输出，否则真实 FMOD 会重复使用旧 tag。
完成计数以最多 10 ms 的宿主轮询驱动释放事件，音频回调不进入 SDK、不获取 IPC 锁。
停止/挂起真实暂停输出，关闭先排空回调再释放所有权；O0/O2 覆盖真实静音 PCM 完成、
tag 复用、事件、音量、生命周期、非法描述符和端点关闭。不宣称已听到游戏声音或验证设备音频。
未观测的音频命令（包括 flush）仍明确拒绝，不提供成功空桩。
AppKit 键盘与 UIKit 按钮现按公开 Horizon FullKey/SystemExt 共享 ring 发布输入，
仅连接已支持的 ID 0/FullKey；未配置前端时保持断开，不声称存在物理 Switch 手柄。
O0/O2 核对采样编号、两种 ring、40 次更新回绕、属性、模拟量范围、抬起与只读 guest alias，
不等同原生 SDK 并发读取正确性的验证；真实菜单消费另行验证。
Mac 用方向键发布左摇杆（不同时发布 D-pad），Return=A、Escape=B、X=X、Z=Y、Q/E=L/R、Space=ZL、P=+；
iOS 有对应按钮。参考 D2R，对已核对的实际导出振动设备查询返回虚拟前端真实的零执行器能力，
不让 SDK 按 FullKey 样式推断不存在的物理震动设备；非法参数明确拒绝，不替换振动初始化为空成功。
HID 命令 203 实际是创建振动设备列表对象，并非权限布尔值；它仍未实现。
权限查询 205 才返回无虚拟震动能力。配置虚拟手柄后原生 SDK 自行跳过手柄选择小程序，
45 秒本机运行提交 4252 次 present 并持续回收 PCM；未配置输入时仍明确停止在库小程序事件。
随后显式读取本进程拥有的 swapchain 纹理，核对真实 1280×720 主菜单。
只有捕获模式附加 TRANSFER_SRC 并等待渲染队列，正常运行不增加这些操作。
图片始终留在本机，Apple Vision 仅输出预设完整词标记的布尔值，不输出图像或识别原文。
受控按键已验证主菜单→选项（CONTROLS/CREDITS）→返回主菜单。
独立新存档运行 125 秒/14546 次呈现，调整原生 layer 与客体固定 1280×720 一致后
SUBOPTIMAL 为零；后期非黑场景并不证明移动、战斗、像素正确性或 iOS 可玩。
库小程序、经过验证的试玩、
RO 传输与生命周期仍未完成。
NIFM 采用有初始化/提交拒绝/取消状态的显式本机离线策略，不宣称有 Internet 能力。
原生 socket 初始化已通过上述真实受保护传输池；没有照搬 D2R 的成功空桩剪口。

生成的 iPhoneOS ARM64 Mach-O 对象中，15 个嵌入段的字节和布局均通过核对。
独立 iPhoneOS App 另通过链接后段字节、对齐、权限及本机 ad-hoc 完整性签名检查；
没有完成 Apple 开发/分发签名、设备安装或启动。
本机具备 Xcode/iPhoneOS SDK 27.0，
但缺少 Metal Toolchain 和有效签名身份。未经批准不安装 Xcode 组件或修改全局工具链。

收据（`extraction.json`、`dependencies.json`、`build/rewrite.json`、
`build/rewrite-dynamic.json`、`build/embed.json`、`build/shaders-msl/shaders.json`、
`build/runtime-core/runtime.json`、`build/runtime-ipc-verified/runtime.json`、
`build/runtime-copy-threads-verified/runtime.json`，
或指定的新探针输出目录内的 `runtime.json`）和详细验证日志保留在
私有外部工作区中，可能包含专有接口元数据和本机路径，不应公开发布。
游戏文件、密钥、着色器输出、对象和安装包全部留在 git 和上传之外。

下一工程阶段不需要等待 Metal 安装：核对菜单之外的实际玩法、手柄支持/库小程序后端、
必需服务及 `ldr:ro` 传输，再沿实际 Vulkan 启动推进。
设备签名和 iPhoneOS
metallib 只阻塞对应设备阶段，不能替代尚未实现的兼容层。

## iOS 真机验证 IPA

`package_ios.py` 使用现有 Xcode/iPhoneOS SDK 直接构建独立 UIKit App，
不运行 D2R iOS 工程、不继承其地址、DLC、HUD、云存档或托管下载策略。
App 包内含真实五模块 AOT 映像、启动所需原始 NRR/NRO、72 份 MSL 和已核对的绑定目录，
完整 RomFS 独立打包供本机传输；
不是仅显示说明文字的空壳，但**也不是可玩的 Hades 安装包**。

```sh
"$PY" hades/tools/package_ios.py --work "$WORK" \
  --bindings "$WORK/build/runtime-vulkan-current-verified/bindings/bindings.json" \
  --out "$WORK/build/ios26-vulkan-package" \
  --reuse-romfs-archive "$WORK/build/ios-side-load-package/Hades-RomFS-local-only.zip" \
  --moltenvk "$MOLTENVK_ROOT"
```

输出必须是 git 之外的新私有目录。脚本核对全部 15 个链接嵌入段字节、16 KiB 对齐、
单一可执行 `__TEXT`、无 RWX 段、iPhoneOS/ARM64/PIE 元数据、资源复制哈希，
以及 IPA 每个解压后条目的哈希。`package.json` 记录源码及包哈希，并明确标记
无 Apple 开发/分发签名、未验证设备执行和未验证真机游戏入口。
新版面向 iOS 26 / iPhone 15 Pro Max / A17 Pro，使用通用 ARM64，不继承 D2R 的
Apple A18 编译参数；默认启用显式宿主符号接管。
可选复用参数会把既有资源 ZIP 的每个文件重新与当前 RomFS/清单核对，
随后引用原 ZIP，不再次压缩或复制数 GB 资源。
着色器用设备系统 Metal `newLibraryWithSource` 编译，不依赖缺失的 Metal CLI；
没有安装全局 Xcode 组件。

完整资源直接放进 App 会约 4.31 GB，超过 Apple 文档的 4 GB 未压缩应用构建上限。
因此最终 `HadesPortTest-resign-required.ipa`（包含 Vulkan 静态运行时，精确体积见 `package.json`）
只携带启动/验证必需资源，
另提供 `Hades-RomFS-local-only.zip`（约 3.9 GB），不使用 D2R 的在线托管资源方案。
脚本强制 App 未压缩体积不超过保守的 4,000,000,000 字节。
为完整资源解压、传输、安装与缓存建议至少预留 10 GB 空间。IPA 需要使用用户自己的 Apple
身份在**本机**重签并侧载；本机 ad-hoc 签名不能直接在未越狱设备安装。
不要将 IPA、游戏资产或密钥上传到在线签名服务。
请求 `increased-memory-limit` 和 `extended-virtual-addressing`，但描述文件必须实际
授权这些权限；不能强行保留未经授权的 entitlement。部分侧载工具/免费账号可能移除
权限，因此能否成功映射及执行必须以真机结果为准。

`sign_ios.py` 提供完全本机的重签链，使用明确指定的本机有效证书 SHA-1、
用户自己的 `.mobileprovision` 和目标手机的描述文件 UDID（不是 CoreDevice 发现 UUID）。
先核对证书、团队、应用 ID、到期日、设备名单及内存授权，再验证原 IPA 的完整哈希/条目，
在新的私有目录复制并签名，不修改原包；最后核对实际签名 entitlement 与 15 个嵌入段。
缺有效身份时在复制游戏前停止，不扫描个人目录、不注册 Apple 账号、不上传包。

```sh
python3 hades/tools/sign_ios.py \
  --work "$HADES_ROOT" --package "$HADES_ROOT/build/ios-side-load-package" \
  --out "$HADES_ROOT/build/local-apple-signed" \
  --profile "/明确指定的本机描述文件.mobileprovision" \
  --identity "<本机有效签名证书的40位SHA-1>" \
  --device "<描述文件中的目标手机UDID>" --bundle-id "<描述文件授权的应用ID>"
```

参考 D2R `dm/ios/run.sh` 的免费账号分支，个人团队不能签扩展内存权限。
本工具默认明确拒绝未经授权的权限，不静默删除；只有显式添加
`--diagnostic-without-memory-entitlements` 才生成缺少相关授权的**诊断包**，
并在私有 `signing.json` 记录删减项目。D2R 的普通地址空间运行结论不自动适用于 Hades：
这里的 2 GiB 堆策略、实际 RSS 和设备地址空间仍需真机核对。
六项合成测试覆盖授权、过期/错设备/错证书、IPA 篡改和实际签名 entitlement 不匹配拒绝；
没有有效 Apple 身份时不能声称真实 Apple 重签或安装成功。

### 在 Xcode 选择自己的开发团队

`xcode_project.py` 把已验证的同一 Hades 目标接成私有原生 `.xcodeproj`，
不是包装预编译游戏的空壳。它直接编译现有运行时、`manifest.c` 和 `images.S`，
链接 pinned MoltenVK，复制已核对的启动资源；不会继承 D2R 的账号、地址或资源下载扩展。
创建前校验源码与资源收据，不下载依赖、不注册设备、不代登录 Apple 账号。

```sh
python3 hades/tools/xcode_project.py \
  --package "$HADES_ROOT/build/ios-side-load-package" \
  --out "$HADES_ROOT/build/xcode-handoff" \
  --moltenvk "/明确指定的本机官方MoltenVK产物目录"
open "$HADES_ROOT/build/xcode-handoff/HadesPortTest.xcodeproj"
```

在 Xcode 的 HadesPortTest target → Signing & Capabilities 选择自己的 Team，
将 Bundle Identifier 改为该团队可用的唯一 ID。通过 USB 连接、解锁并信任手机，
在手机开启开发者模式，选择实际 iPhone 而非模拟器后 Run。
个人团队应在**生成时**显式添加 `--diagnostic-without-memory-entitlements`，
它只保证不请求两个受限内存权限，不保证 2 GiB 客体堆能在设备运行。
完整权限模式要求团队的描述文件实际授权这些能力，不要任由工具静默移除。
第 8 版将源码、头文件与生成的调用/布局源冻结成只读快照，不引用可变 worktree 源码。
签名团队和 Bundle ID 仍可在工程中配置。嵌入二进制、启动资源与 pinned 框架仍以
绝对本机路径引用，保持这些私有目录可用且不要分享工程包。

本机已使用真实 `xcodebuild` 构建 ARM64/iOS 26 目标，无签名构建核对全部 15 个
嵌入段及保护属性、296 份启动/着色器/图标资源的精确字节。图标用直接复制阶段避免
Xcode PNG 处理改变已核验字节；不通过忽略差异来宣称一致。
这仍不是 Apple 开发签名、安装、A17 Pro 运行或试玩证据。

原生核心/GPU/SDK 三项诊断不需要先导入完整资源包。新增第四项真实游戏入口测试
需要在本机解压资源 ZIP，
通过 USB 文件共享把 `RomFS` 文件夹放入 App 的 `Documents/RomFS`；
启动前核对每个资源的名称、大小与 SHA-256，不降级使用不完整的包内资源。
`Documents/HadesState` 存放实际持久离线用户与本地存档目录。
安装后依次运行“原生映像/EOS/计时门”和“72 份 MSL/离屏读回”，再运行真实 SDK 启动。
前两项分别应产生 `CORE_OK`、`MSL_COMPILE_OK`、三条 `FRAME_OK` 和 `RENDER_OK`；
这些仍不是游戏帧。SDK/游戏初始化参考 D2R 使用专用 64 MiB pthread，
不再放在可被复用的 GCD 线程上；返回后保持线程、TSD 与客体状态直至进程退出，
不能在同一进程重复初始化。本机带输入路线已跨过分配回调并验证菜单像素和操作，
这不等同 iOS 已运行。第 8 版声明单应用全屏并请求横屏，以固定 1280×720 的 16:9 Metal 视图
呈现，叠加 12 个触控按钮和“日志”按钮；尚未验证设备实际画面或设备输入。
“日志”会释放已按下的虚拟按键、显示可滚动诊断界面，但不会暂停客体；
“返回游戏画面”恢复视图。场景失去/恢复前台焦点发布 CommonState 的实际状态 2/1
和通知 15，并释放已按下的按键；重复状态不产生伪通知，需要线程暂停的策略明确拒绝。
资源检查或启动失败会恢复诊断界面。日志视图最多读取末尾 128 KiB，导出保留完整日志。
默认保留首次符号调用、服务命令、前四次等待与周期呈现计数，错误仍完整报告；
本机启动前设置 `HADES_TRACE=1` 可恢复完整 dispatch/IPC 跟踪（`0` 为默认值）。
重新打开 App 可刷新/导出 `Documents/hades-device.log`；
也可通过本机 USB 文件共享取得日志。不自动上传，不记录密钥或 Apple 账号。
Hades 实际请求 `0/0/1` 焦点策略，明确要求后台客体线程暂停。
参考 D2R，完整恢复还需禁止新的 GPU 提交、排空在途工作并冻结客体时间；
只有焦点布尔值和音频暂停不够。目前不能伪称这些已完成。
因此第 8 版 GPU/SDK/游戏诊断活动期间切后台会打印 `FOREGROUND_ONLY_ABORT`
并以状态 75 终止整个诊断进程，防止继续后台 GPU 提交；空闲/仅核心测试不触发。
这不是暂停/恢复或强制保存，未提交进度可能丢失。使用诊断版时必须保持前台。
真机要核对签名页 remap、Apple TSD 布局、小客体栈/宿主门栈和 GPU 兼容性，
然后再据实际日志推进必需服务、客体图形/呈现、输入和声音；目前仍不是试玩版。
