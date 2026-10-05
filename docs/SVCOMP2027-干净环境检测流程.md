# SV-COMP 干净环境完整检测流程（考官视角，从头到尾）

> 用途：在一台**全新的 Ubuntu 24.04**（实体机/VM）上，模拟考官从拿到归档到跑出最终分数
> 的完整链路。全新 VM 本身就是干净环境，**不需要 Docker**——只要只装那 22 个运行时包
> （不含 gcc/clang/APRON），就等价于 SV-COMP 的容器环境。预期最终分数：**1503**
> （TP=85 / FP=0 / TN=709 / FN=0 / unknown=235）。

---

## 阶段 0：全新 Ubuntu 24.04，只装 22 个运行时包（无编译器）

```bash
sudo apt update
sudo apt install -y unzip python3 python3-venv \
  libc6-dev libc6-dev-i386 lib32gcc-13-dev \
  libgmp10 libgmpxx4ldbl libmpfr6 libppl14 \
  libboost-filesystem1.83.0 libboost-thread1.83.0 libtbb12 \
  libsqlite3-0 libtinfo6 libz1 libz3-4 libicu74 libxml2 libffi8 \
  libedit2 libbsd0 liblzma5 libmd0
```

> 关键：**不装 gcc、不装 clang、不装 APRON**。这 22 个包只是运行时库；clang/APRON/Z3
> 都打包在归档里。这样 VM 环境 ≈ SV-COMP 的容器环境。

---

## 阶段 1：装 BenchExec（真评测框架）

```bash
python3 -m venv ~/venv-benchexec
source ~/venv-benchexec/bin/activate
pip install benchexec
benchexec --version
```

---

## 阶段 2：拿两样东西（考官视角 = 从 Zenodo + benchmark 仓库拿）

1. **归档**（模拟从 Zenodo DOI 下载）：把 `ikos-conc.zip` 拷进来（或真的从 Zenodo 下）。
2. **基准集**：拷入 `sv-benchmarks-2026`（含 `.yml` / `.i` 文件）。

---

## 阶段 3：解压归档 + 直接跑 smoketest（等价于容器里的 smoketest）

```bash
cd ~/examiner
unzip ikos-conc.zip -d tool/
cd tool/ikos-conc && ./smoketest.sh
```

**通过标志**：最后一行 `The program is definitely UNSAFE`、exit 0。

> 这一阶段验证：在「只有 22 个包、无编译器」的环境里，归档自包含、能跑通。
> （Docker 版见文末「可选：复刻容器化」）

---

## 阶段 4：BenchExec 跑全量基准（真评测）

### 4.1 把 tool-info 模块放进 BenchExec 包

```bash
cp <仓库>/benchexec/tools/ikos-conc.py \
   ~/venv-benchexec/lib/python3.12/site-packages/benchexec/tools/ikos-conc.py
```

### 4.2 补 property 文件（benchmark 里缺）

```bash
mkdir -p <benchmark根>/c/properties
printf 'CHECK( init(main()), LTL(G ! data-race) )\n' > <benchmark根>/c/properties/no-data-race.prp
# 其余 .yml 引用的 prp（unreach-call/valid-memsafety/no-overflow/termination/valid-memcleanup/
# def-behavior/coverage-*）也要放进去：前 6 个从 sv-witnesses 仓库 linter/test/properties/ 拷贝，
# coverage-* 自建最小内容即可（如 COVERAGE( init(main()), FQL(COVER EDGES(@BASICBLOCKENTRY)) )）。
```

### 4.3 生成全量任务清单 + 任务定义 XML

```bash
# tasks-full.txt：所有 no-data-race 的 .yml 绝对路径（用仓库 svcomp_race_test.py 扫描生成，共 1029 行）
cat > benchmark-full.xml <<'EOF'
<?xml version="1.0"?>
<benchmark tool="ikos-conc">
  <tasks>
    <includesfile>tasks-full.txt</includesfile>
    <propertyfile><benchmark根>/c/properties/no-data-race.prp</propertyfile>
  </tasks>
  <rundefinition name="svcomp"/>
</benchmark>
EOF
```

### 4.4 跑全量

```bash
export PATH=$HOME/examiner/tool/ikos-conc/bin:$PATH   # 指向解压归档的 bin/ikos
source ~/venv-benchexec/bin/activate
benchexec --timelimit 900s --memorylimit 15GB benchmark-full.xml
```

### 4.5 生成官方同款表

```bash
table-generator results/benchmark.*.results.*.xml.bz2
```

**预期统计**：

```
Statistics: 1029 Files
  correct true:   709   (+2 分)
  correct false:   85   (+1 分)
  incorrect:        0
  unknown:        235
  Score:         1503
```

---

## 阶段 5：见证验证（validator）

对每个 **FALSE** 的 `witness.yml`，用 CPAchecker 复核：

```bash
cd <cpachecker主分支>
export JAVA_HOME=<jdk21> PATH="$JAVA_HOME/bin:$PATH"
scripts/cpa.sh \
  --witness <某文件的witness.yml> \
  --spec <no-data-race.prp> \
  --option witness.validation.violation.config=config/svcomp27--datarace.properties \
  <对应的.c/.i文件>
```

- `Verification result: FALSE` = 见证被确认。
- 全量结果：85 个 FALSE 里约 40 个确认、45 个「CPAchecker 自己算不出（UNKNOWN）」。

> ⚠️ 这 45 个「未确认」如何计分，需向 SV-COMP 主席 Dirk 确认（这是唯一实质不确定项）。

---

## 阶段 6：最终结论

若 4.5 的 `Score = 1503`，则「考官拿到归档 → 解压 → smoketest → 真 BenchExec 跑全量 → 官方统计」
整条链路验证通过，与本地 `svcomp_race_test.py` 的 1503 完全一致。

---

## 附（可选）：用 Docker 复刻 SV-COMP 的容器化机制

只有想 100% 复刻「FM-Tools 建镜像 + BenchExec 容器模式」时才需要。它验证的是 SV-COMP 的打包
基建，不是我们的工具本身；工具只要在「22 包干净环境」能跑就行（上面阶段 3 已验）。

```bash
sudo apt install -y docker.io && sudo systemctl enable --now docker && sudo usermod -aG docker $USER
# 退出重登

cat > Dockerfile <<'EOF'
FROM docker.io/ubuntu:24.04
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      unzip libc6-dev libc6-dev-i386 lib32gcc-13-dev libgmp10 libgmpxx4ldbl libmpfr6 libppl14 \
      libboost-filesystem1.83.0 libboost-thread1.83.0 libtbb12 libsqlite3-0 libtinfo6 libz1 libz3-4 \
      libicu74 libxml2 libffi8 libedit2 libbsd0 liblzma5 libmd0 python3 && rm -rf /var/lib/apt/lists/*
COPY ikos-conc.zip /tmp/ikos-conc.zip
RUN cd /opt && unzip -q /tmp/ikos-conc.zip && rm /tmp/ikos-conc.zip
WORKDIR /opt/ikos-conc
EOF

docker build -t ikos-conc:svcomp27 .
docker run --rm ikos-conc:svcomp27 ./smoketest.sh
```
