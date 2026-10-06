# SV-COMP FAQ 中文翻译

> 原文：
>
> [https://sv-comp.sosy-lab.org/faq.txt](https://sv-comp.sosy-lab.org/faq.txt)
>
> （跨年份共用的纯文本 FAQ）
> 术语：verifier = 验证器，witness = 见证，benchmark definition = 基准定义，tool-info = 工具信息模块。
> 译者注：FAQ 部分内容较早（含 
>
> `.graphml`
>
>  旧格式、2017 年链接），
>
> **机制性回答仍然适用，具体格式 / 链接以 2027 页面为准**
>
> 。



***

## 一、可复现性（Replicability）

### Q1：如何确认我的验证器被正确执行？团队收到结果后应该做什么？

**A：** 第一步是做以下健全性检查（sanity checks）：



* 验证器是否正常启动？

* 日志文件是否可访问？

* 见证文件是否可访问？

* 验证器是否找到了所有需要的组件？

### Q2：类别 opt-out 是否使用正确？验证器所需的选项是否设置正确？

**A：** 参赛者有责任确保基准定义正确。该 VERIFIER 实际使用的基准定义位于仓库：

[https://gitlab.com/sosy-lab/sv-comp/bench-defs/-/blob/main/benchmark-defs/VERIFIER.xml](https://gitlab.com/sosy-lab/sv-comp/bench-defs/-/blob/main/benchmark-defs/VERIFIER.xml)

### Q3：我的验证器是否正确安装在竞赛机器上？结果是否被正确解释？

**A：** 参赛者有责任确保 BenchExec 的 tool-info 模块正常工作。该模块规定：验证器归档中的哪些路径应安装到竞赛机器上，以及如何解析、翻译日志中的结果。

该 VERIFIER 实际使用的 tool-info 模块位于 BenchExec 仓库：

[https://github.com/sosy-lab/benchexec/tree/main/benchexec/tools/VERIFIER.py](https://github.com/sosy-lab/benchexec/tree/main/benchexec/tools/VERIFIER.py)

请务必按照说明测试你的 tool-info 模块：

[https://github.com/sosy-lab/benchexec/blob/main/doc/tool-integration.md#testing-the-tool-integration](https://github.com/sosy-lab/benchexec/blob/main/doc/tool-integration.md#testing-the-tool-integration)

### Q4：在哪里可以找到 SV-COMP 使用的验证任务？

**A：** SV-COMP 的验证任务位于开放的 SV-COMP 仓库：

[https://gitlab.com/sosy-lab/benchmarking/sv-benchmarks/-/tree/main/c](https://gitlab.com/sosy-lab/benchmarking/sv-benchmarks/-/tree/main/c)

组织者和评审团把验证任务划分为各类别，类别说明见：[https://sv-comp.sosy-lab.org/2027/benchmarks.php](https://sv-comp.sosy-lab.org/2027/benchmarks.php)

### Q5：在竞赛机器上，我的验证器能看到哪些目录？

**A：** 概念性描述见论文《Reliable Benchmarking: Requirements and Solutions》（STTT 2017）；技术细节见帮助页（`runexec -h`）。

下面是一个 BenchExec 执行器（runexecutor）的示例调用及其说明：



```
python -m benchexec.runexecutor

&#x20; \--container

&#x20; \--read-only-dir /

&#x20; \--hidden-dir /home

&#x20; \--hidden-dir /sys/kernel/debug

&#x20; \--hidden-dir /var/lib/cloudy

&#x20; \--overlay-dir /etc

&#x20; \--overlay-dir /tmp/.../working\_dir

&#x20; \--dir /tmp/.../working\_dir/bin-2018/utaipan

&#x20; \--output /tmp/.../working\_dir/cloudBenchmarkOutput.txt

&#x20; \--output-directory /tmp/.../working\_dir/bin-2018/utaipan

&#x20; \--debug

&#x20; \--maxOutputSize 2000000

&#x20; \--timelimit 60

&#x20; \--memlimit 7000000000

&#x20; \--memoryNodes 0

&#x20; \--cores 3,7

&#x20; \--

&#x20; ./Ultimate.py

&#x20; \--spec ../../sv-benchmarks/c/Termination.prp

&#x20; \--file ../../sv-benchmarks/c/product-lines/minepump\_spec1\_product06\_true-unreach-call\_false-termination.cil.c

&#x20; \--full-output

&#x20; \--architecture 32bit
```

**说明：**



* 以 BenchExec 的容器模式启动执行器（runexecutor）；

* 容器中所有文件默认只读，以下情况除外；

* 隐藏 `/home` 目录，在容器内提供一个全新副本；`/sys/kernel/debug`、`/var/lib/cloudy` 同理；

* `/etc` 的内容在容器内可见，所有写操作重定向到一个全新目录；

* 工作目录由 `--dir` 的值指定；

* 验证器的全部输出（stdout 和 stderr）写入 `--output` 指定的文件；

* 写入 `.` 目录的文件（`--result-files` 的默认值）会被复制到主机上 `--output-directory` 指定的目录（即用户可获取）；

* 生成调试信息；

* 按 SV-COMP 规则，输出上限为 2 MB；

* 设置时间限制（60 秒为预跑阶段）、内存限制（7 GB 为预跑阶段）；

* 使用内存节点 0；使用编号 3、7 的 CPU 核（预跑限制为 2 核）；

* 其余参数与具体验证器有关。



***

## 二、结果（Results）

### Q6：在哪里可以找到我的验证器输出？

**A：** HTML 结果表中的 "状态（status）" 列包含日志文件链接。如果结果与预期不符，追踪问题所需的信息都在日志里。请确保日志中打印了足够多、有助于理解运行结果的细节。

### Q7：上一次验证运行使用的是我的哪个版本？

**A：** 确切版本包含在每张结果表的表头中 ——BenchExec 会要求 tool-info 模块生成验证器的版本字符串。此外，好的验证器会在开头打印精确的版本信息，例如：

`This is Verifier BEST version 3.14 compiled 2016-11-27T17:25.`

这样日志里也能看到版本信息。

### Q8：验证器需要的所有组件是否都被正确找到？组件 X 使用的是哪个版本？

**A：** 检查验证器产生的日志，任何重要的异常都应有一行输出。验证器也可以打印组件（gcc、llvm 等）的确切版本，便于事后确认使用了哪些组件。

### Q9：验证运行后会提供哪些数据？

**A：** 每次验证运行后，参赛团队会收到一封邮件，含其验证器结果的链接。



* XML 文件是 BenchExec 产生的原始数据；

* HTML 表为了方便浏览，信息较少；需要更多细节时请回看 XML 文件；

* XML 文件可用 BenchExec 的 tablegenerator 方便地转换为自定义 HTML 页面；

* 文件名含 "merged" 的 XML/HTML 文件用于做了见证核验的类别；对于强制要求见证核验的类别，状态（结果）会根据见证核验结果进行修正，表中还会有见证核验过程的附加列；见证核验会用所有可用的见证核验器执行。

为方便检查结果，表中有若干新特性：



* 验证任务名称列含验证任务源码的链接；

* "wit" 列含验证器所产见证的链接；

* "inspect" 列含见证检查页的链接；

* "status" 列含验证器所产日志的链接；

* "wit\*\_status" 列含见证核验所产日志的链接。



***

## 三、见证核验（Witness Validation）

### Q10：如何查看我的见证信息？

**A：** 点击 "inspect" 链接，会打开一个单独页面，提供该见证的更多信息。

### Q11：某程序的示例见证在哪里？

**A：** 见证检查页包含其他验证器为同一程序产出的见证列表。

### Q12：如何自己核验我的见证？

**A：** 见证检查页提供链接，可使用 CPAchecker 的在线核验服务；也可以下载可用的核验器安装到自己机器上运行。

### Q13：验证器产出的见证是否被找到并送入核验？看起来我的见证没被找到。

**A：** "wit" 列含你的见证链接，点击可确认它是否可用。检查你的见证文件格式是否正确、必填字段是否齐全。

> 译者注：此条为旧内容（要求 
>
> `.graphml`
>
>  且只允许一个该文件）。
>
> **现行格式为 YAML（文件名 witness.yml），数据竞争类别要求版本 2.2，详见 2027 规则页。**

### Q14：SV-COMP 从什么时候开始要求产出验证见证？

**A：** 错误见证一直都要求。



* 从 SV-COMP 2015 起，违例见证要求机器可读、可复核，以便组织者判断一个违例见证是否为误报。这是社区的决定，仅适用于 "FALSE" 结果；"TRUE" 当时仍可无见证。

* SV-COMP 2014 设有一个见证核验演示类别，试验了该格式并开发出前两个见证核验器。

* 从 SV-COMP 2017 起，"TRUE" 结果也要求提供见证（在 2016 年作为演示类别测试之后）。

### Q15：如果我的验证器产出了错误的见证（或没有见证），即没有可用的核验器能核验它，会有什么处罚？

**A：** SV-COMP 中每个验证任务的答案都会获得一个分数（见规则）。如果没有任何见证核验器能核验你的见证，它会被记为 **"unconfirmed（未确认）"**。对于正确性见证，未确认的结果按较低分值处理（见计分规则）。

### Q16：验证运行的结果如何获得、分数如何计算？

**A：** SV-COMP 用于可靠基准测试的执行平台是 BenchExec：[https://github.com/sosy-lab/benchexec](https://github.com/sosy-lab/benchexec)

用 BenchExec 获得结果和测量数据后，按规则页 "Evaluation by Scores and Runtime" 一节中的表格计分；单个验证任务的计分算法在该页有图示；元类别的加权得分按规则页的定义计算。

### Q17：我可以贡献自己的见证核验器吗？

**A：** 完全可以。SV-COMP 社区非常需要见证核验器。如果你有，请发给组织者。如果最终有超过 2–3 个见证核验器，评审团可能会做选择，或同时应用多个核验器。

### Q18：目前有多少个基于见证的结果核验器？

**A：** 目前有三个项目在做结果核验器：CPAchecker、FShell、Ultimate Automizer。

> 译者注：此为旧数据；目前核验器生态已扩展（Witch、Dartagnan、Goblint、Theta、MetaVal 等，其中并发类别的核验以 Dartagnan 为主）。

### Q19：见证格式是谁提出的？

**A：** 见证格式由开发见证核验器的人员制定，他们在 SV-COMP 2014 的演示类别中进行了试验。此后不断有扩展和改进被提出并纳入。

### Q20：哪些类别参与结果核验？

**A：** 某些验证任务被排除在结果核验之外，细节见 2027 规则页的见证格式表。

### Q21：见证核验器 X 无法重新验证某个验证任务，因此不应该做见证核验。

**A：** 见证核验一定会进行，这一决定不会撤销，尤其是在 2015、2016 年成功应用之后。如果你偏好的核验器不工作，要么换一个见证核验器，要么申请把该验证任务从 SV-COMP 中排除。

### Q22：我的验证器想用某个特定提示来引导见证核验器，可以吗？

**A：** 可以，只需确保其中一个见证核验器能消费它。