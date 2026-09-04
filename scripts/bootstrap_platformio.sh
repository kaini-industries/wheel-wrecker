#!/bin/sh
set -eu

script_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
repository_root=$(dirname -- "$script_dir")
venv_path=${PLATFORMIO_VENV:-"$repository_root/.venv-platformio"}

case "$venv_path" in
  /*) ;;
  *) venv_path="$repository_root/$venv_path" ;;
esac

is_supported_python() {
  "$1" -c 'import sys; raise SystemExit(0 if (3, 10) <= sys.version_info[:2] <= (3, 13) else 1)' \
    >/dev/null 2>&1
}

find_supported_python() {
  if [ -n "${PLATFORMIO_PYTHON:-}" ]; then
    if command -v "$PLATFORMIO_PYTHON" >/dev/null 2>&1 && \
      is_supported_python "$PLATFORMIO_PYTHON"; then
      command -v "$PLATFORMIO_PYTHON"
      return 0
    fi
    echo "PLATFORMIO_PYTHON must select Python 3.10 through 3.13: $PLATFORMIO_PYTHON" >&2
    return 1
  fi

  for candidate in python3.13 python3.12 python3.11 python3.10; do
    if command -v "$candidate" >/dev/null 2>&1 && \
      is_supported_python "$candidate"; then
      command -v "$candidate"
      return 0
    fi
  done

  echo "This project runs PlatformIO with Python 3.10 through 3.13." >&2
  echo "No supported interpreter was found." >&2
  echo "Install Python 3.13 or set PLATFORMIO_PYTHON to a supported interpreter." >&2
  return 1
}

platformio_is_ready() {
  [ -x "$venv_path/bin/python" ] && \
    is_supported_python "$venv_path/bin/python" && \
    "$venv_path/bin/python" -c \
      'import platformio; raise SystemExit(0 if platformio.__version__ == "6.1.19" else 1)' \
      >/dev/null 2>&1
}

if platformio_is_ready; then
  exit 0
fi

if [ -e "$venv_path/bin/python" ] && \
  ! is_supported_python "$venv_path/bin/python"; then
  echo "Existing project PlatformIO environment is not using Python 3.10-3.13: $venv_path" >&2
  echo "Remove that project-local directory and run this script again." >&2
  exit 1
fi

python_path=$(find_supported_python)
if [ ! -x "$venv_path/bin/python" ]; then
  echo "Creating project-local PlatformIO environment with $python_path"
  "$python_path" -m venv "$venv_path"
fi

echo "Installing pinned PlatformIO into $venv_path"
"$venv_path/bin/python" -m pip install --disable-pip-version-check \
  --requirement "$repository_root/requirements-platformio.txt"

if ! platformio_is_ready; then
  echo "PlatformIO installation did not produce the expected 6.1.19 environment." >&2
  exit 1
fi
