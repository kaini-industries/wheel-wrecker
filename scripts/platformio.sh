#!/bin/sh
set -eu

script_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
repository_root=$(dirname -- "$script_dir")
venv_path=${PLATFORMIO_VENV:-"$repository_root/.venv-platformio"}

case "$venv_path" in
  /*) ;;
  *) venv_path="$repository_root/$venv_path" ;;
esac

PLATFORMIO_VENV="$venv_path" "$script_dir/bootstrap_platformio.sh"
exec "$venv_path/bin/pio" "$@"
