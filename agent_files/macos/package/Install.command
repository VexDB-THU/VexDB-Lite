#!/bin/bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")" && pwd)"
/bin/bash "$ROOT/install.sh"
echo ""
read -r -p "安装完成，按回车关闭窗口。" _
