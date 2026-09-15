#!/usr/bin/env bash
# ============================================================================
# 从 VMware 客户机拉取 lianxi 工程 → 同步进本仓库 → git 提交
#
#   为什么要有这个脚本：
#     客户机（Ubuntu 18.04）里**没装 git、也没有外网**，工程本身无法做版本管理，
#     而且工程只存在于虚拟机里（VM 坏了就全没了）。所以这里放一个"主机侧镜像仓库"，
#     每次在客户机改完代码，在主机（git-bash）里跑一下本脚本就能留档 + 异地备份。
#
#   用法（在 git-bash 里）：
#     cd /c/Users/Lenovo/source/repos/ModbusGateway
#     bash _sync_from_vm.sh "这次改了什么"
#     bash _sync_from_vm.sh              # 不带说明就用时间戳当提交信息
#
#   前置条件：VMware 里那台 Ubuntu 客户机**开机运行中**（不需要客户机有网络，
#             走的是 VMware Tools 通道，所以客户机网卡断了也能用）。
# ============================================================================
set -u
export MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL='*'

VMRUN="/c/Program Files (x86)/VMware/VMware Workstation/vmrun.exe"
VMX='F:\华清远见\18jiaoxue-2024.6.26\Ubuntu 64 位.vmx'
GU=root
GP=1
REPO="$(cd "$(dirname "$0")" && pwd)"
GUEST_TAR=/tmp/lianxi_sync.tar.gz
HOST_TAR_WIN='C:\Users\Lenovo\AppData\Local\Temp\lianxi_sync.tar.gz'
HOST_TAR=/c/Users/Lenovo/AppData/Local/Temp/lianxi_sync.tar.gz
MSG="${1:-sync $(date +'%Y-%m-%d %H:%M')}"

# 同步时保留在仓库里的"仓库自己的文件"（不属于客户机工程）
KEEP=("_sync_from_vm.sh" "README_镜像仓库.md")

[ -x "$VMRUN" ] || { echo "[sync] 找不到 vmrun：$VMRUN"; exit 2; }
cd "$REPO" || { echo "[sync] 进不去仓库目录：$REPO"; exit 2; }

echo "[1/4] 客户端打包（排除编译产物/数据库/日志）…"
"$VMRUN" -T ws -gu "$GU" -gp "$GP" runProgramInGuest "$VMX" /bin/bash -c \
  "cd /home/hq/network && rm -f $GUEST_TAR && tar czf $GUEST_TAR \
   --exclude='*.out' --exclude='sensor.db*' --exclude='logs' --exclude='run' \
   --exclude='__pycache__' lianxi" \
  || { echo "  ✗ 打包失败：VM 没开机？VMware Tools 没运行？"; exit 3; }

echo "[2/4] 拉回主机…"
rm -f "$HOST_TAR"
"$VMRUN" -T ws -gu "$GU" -gp "$GP" copyFileFromGuestToHost "$VMX" "$GUEST_TAR" "$HOST_TAR_WIN" >/dev/null \
  || { echo "  ✗ 拉取失败"; exit 4; }

echo "[3/4] 同步进仓库工作区（保留仓库自己的辅助文件）…"
EXCL=()
for k in "${KEEP[@]}"; do EXCL+=(! -name "$k"); done
find . -mindepth 1 -maxdepth 1 ! -name .git "${EXCL[@]}" -exec rm -rf {} +
tar xzf "$HOST_TAR" -C . --strip-components=1 || { echo "  ✗ 解包失败"; exit 5; }

echo "[4/4] git 提交…"
if [ -z "$(git status --porcelain)" ]; then
  echo "  客户机代码和仓库一致，没有改动，不需要提交"
  exit 0
fi
CHANGED=$(git status --porcelain | wc -l)
git add -A
git commit -q -m "$MSG"
echo "  ✓ 已提交 $(git log -1 --format='%h %s')（改动 $CHANGED 处）"
echo "  最近 5 条历史："
git log --oneline -5 | sed 's/^/    /'
