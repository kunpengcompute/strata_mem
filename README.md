# strata_mem

strata_mem SIG信息

## 固定物理偏移内存接口

C++17 实现，提供 `l3mem::malloc_l3()` 和 `l3mem::free_l3()`。
实际内存申请要求 **Linux 64 位**。非 Linux 平台的申请接口返回空对象，
`errno=ENOTSUP`，不支持运行硬件访问测试。

```text
src/
  l3_mem.h             对外接口和结构体
  l3_mem.cpp           大页映射、初始化、物理地址检查、日志、释放
  l3_mem_internal.h    内部固定偏移表和布局校验
test/
  l3_mem_test.cpp      原程序的线程访问、绑核、预热和吞吐统计
  l3_mem_workload.h    测试专用的访问序列和读取内核
```

### 接口约定

```cpp
#include "l3_mem.h"

auto buffer = l3mem::malloc_l3();
if (!buffer.valid()) {
    // 立即读取 errno，处理申请失败。
} else {
    // 使用 buffer.regions[i].address / size，每项大小为 1MiB。
    // 停止全部访问，并完成外部设备内存注销后：
    if (l3mem::free_l3(buffer.base_address)) {
        buffer = {};
    } else {
        // 检查 errno；不要丢弃原始地址。
    }
}
```

- 单次 `mmap` 映射 **32GiB 连续虚拟地址**，使用32个1GiB HugeTLB大页。
- 保留参考程序的 `MAP_PRIVATE | MAP_ANONYMOUS` 行为和全量32GiB清零。
- 继承调用进程的 NUMA 策略，不在接口内部强制 `mbind` 到 node 0。
- 使用 `/proc/self/pagemap` 获取真实物理地址；逐基础页验证1GiB页内连续性，
  然后按物理地址排序。排序不移动内存，也不改变原始 `base_address`。
- 固化用户提供的 `mode=random seed=35` 日志中全部70项
  `region / sorted_page / offset`，而不是运行时重新生成随机偏移。
- `region=0..69` 顺序不变；排序页0～5各有3个区域，其余各2个。
  256MiB分区计数为 `[18,13,26,13]`。每个区域1MiB，共 **70MiB**。
- 绝对PA/VA不是固定值；大页基地址变化后，L3映射效果仍需重新验证。
- 没有普通页、其他布局或其他大页尺寸的静默回退。
- `free_l3(base_address)` 整体解除32GiB映射；`free_l3(nullptr)` 成功。
  没有全局分配登记，地址对齐检查也不代表所有权检查。
  调用者必须提供有效的原始映射地址，不能传区域地址、重复释放或并发释放。
- 描述结构体可复制，但复制不产生新的分配和释放权；它没有自动析构释放行为。
- 不做SPDK/RDMA注册、不锁定L3、不保证缓存命中率提升。
  有DMA使用时，由适配层在释放前停止请求并注销内存。
- 不建议在分配后 `fork` 并写入私有映射，避免写时复制影响资源和布局。

### 编译

需要支持C++17的编译器、make、ar和Linux pthread；不依赖libnuma。

```sh
make -j
```

产物：

- `build/libl3_mem.a`：接口静态库。
- `build/l3_mem_test`：需要真实大页的访问测试。

其他程序可通过 `-Isrc` 引用头文件，并链接 `build/libl3_mem.a`。
编译不会申请大页；执行 `build/l3_mem_test --help` 仅显示参数说明。

### 服务器运行条件

调用前确认选定NUMA节点上有足够的空闲1GiB大页，并具备HugeTLB、内存策略
和pagemap所需权限（通常需要root/CAP_SYS_ADMIN）。程序不会更改系统大页预留量。
并发运行多个实例会分别占用32GiB，请勿与其他大页业务争抢资源。

```sh
cat /sys/devices/system/node/node0/hugepages/hugepages-1048576kB/free_hugepages
```

首次触碰大页仍可能因资源不足产生 `SIGBUS`，不能把所有资源故障都变成普通
错误返回；程序没有安装SIGBUS恢复处理。正常可检测失败会回滚映射并返回空对象。
如果回滚 `munmap` 本身异常失败，会打印原始地址与长度，需手工处理或退出进程。

### 固定布局访问测试

复现提供的14线程、每线程5个区域、预热10秒、测量10秒场景：

```sh
numactl --membind=0 ./build/l3_mem_test \
  --threads 14 --rounds 1 --warmup 10 --seconds 10 \
  > test.stdout.log 2> test.mapping.log
```

`numactl` 是可选的外部启动工具，不是接口依赖。以上默认绑CPU 0～13，
应按实际CPU权限、NUMA拓扑和SMT关系调整。若CPU不可用，会在申请大页前报错。
显式CPU列表必须与线程数一致，支持逗号分隔的CPU编号，不支持范围语法：

```sh
numactl --membind=0 ./build/l3_mem_test \
  --threads 7 --cpus 0,2,4,6,8,10,12 --warmup 10 --seconds 90 --rounds 3 \
  > test.stdout.log 2> test.mapping.log
```

参数默认值为14线程、1轮、预热10秒、测量90秒；支持7或14线程。
7线程每线程10个区域，14线程每线程5个区域，按返回数组顺序分配，不遗漏区域。
块大小、大页数量、工作集容量和偏移不再作为参数开放。

**删除的是随机选址，不是随机访问。**工作线程仍使用原程序的SplitMix64和
固定线程种子，从自己的区域中随机选一个，然后按64字节步长做1字节volatile读。
每个阶段重新使用相同访问种子，全部线程绑核后同步启动。每轮复用同一次分配。
不再提供 `random/weighted_hist/contiguous` 模式和偏移搜索种子参数。

测试期间收到SIGINT/SIGTERM后，停止并等待工作线程，再释放映射。
若在初始化期间收到信号，需要等初始化调用返回才会清理。
SIGKILL、SIGBUS等非正常退出无法执行用户态清理流程。

### 日志和测量口径

接口在stderr打印32个排序大页、70个区域地址及最终成功标志：

```text
l3mem ready: base=... reserved=34359738368 selected=73400320 regions=70 layout=seed35-fixed-v1 numa=inherited
```

测试程序在stdout打印线程分配和 `TRIAL_BEGIN` / `STAGE_BEGIN` /
`STAGE_END` / `TRIAL_END`。正式采样应对齐 `stage=measure`，
不包含32GiB清零、逐页PA检查和预热。扫描期间缓冲区不再发生重新选址。

- `covered_GiB`：重复扫描的区域跨度累计量，不是DDR流量。
- `covered_GiB_s`：扫描吞吐率，不是内存控制器带宽。
- `load_GiB`：按每64字节执行1字节load计算的源码读取字节量。
- 程序不直接采集L3命中率和DDR带宽，需要外部监控工具。
- 测试按64字节步长访问，并不自动探测硬件缓存行长度。

### 验证范围

当前不附带单元测试；接口中的偏移表编译期检查、申请过程的物理地址检查和
测试程序的参数校验仍然保留。实际HugeTLB分配、NUMA归属、线程调度和硬件
缓存效果需要在Linux服务器上验证。
