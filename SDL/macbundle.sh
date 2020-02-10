#!/bin/bash

PPSSPP="${1}"
PPSSPPSDL="${PPSSPP}/Contents/MacOS/PPSSPPSDL"

if [ ! -f "${PPSSPPSDL}" ]; then
  echo "No such file: ${PPSSPPSDL}!"
  exit 0
fi

SDL=$(otool -L "${PPSSPPSDL}" | grep -v @executable_path | grep -Eo /.+libSDL.+dylib)
if [ "${SDL}" = "" ]; then
  echo "SDL is already bundled/unused."
  exit 0
fi

if [ ! -f "${SDL}" ]; then
  echo "Cannot locate SDL: ${SDL}!"
  exit 0
fi

echo "Installing SDL from ${SDL}..."

SDLNAME=$(basename "${SDL}")
mkdir -p "${PPSSPP}/Contents/Frameworks" || exit 0
cp -r "$SDL" "${PPSSPP}/Contents/Frameworks" || exit 0
install_name_tool -change "${SDL}" "@executable_path/../Frameworks/${SDLNAME}" "${PPSSPPSDL}" || exit 0
