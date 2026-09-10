#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "${script_dir}/.." && pwd)"
build_dir="${repo_root}/build/web-game"
dist_dir="${repo_root}/build/web-dist"
image="emscripten/emsdk:6.0.8@sha256:f174124ff798a3ead1abef247d9a849c270b642d552fea500a42565ff210f765"
build_cpus="${TH08_WEB_BUILD_CPUS:-2}"
build_memory="${TH08_WEB_BUILD_MEMORY:-4g}"
docker_limits=(
    --cpus "${build_cpus}"
    --memory "${build_memory}"
    --memory-swap "${build_memory}"
)

cmake -E make_directory "${build_dir}" "${repo_root}/build/emscripten-cache"

docker run --rm \
    "${docker_limits[@]}" \
    --volume "${repo_root}:/src" \
    --workdir /src \
    --user "$(id -u):$(id -g)" \
    "${image}" \
    env EM_CACHE=/src/build/emscripten-cache \
    emcmake cmake -S /src -B /src/build/web-game -G "Unix Makefiles" \
        -DCMAKE_BUILD_TYPE=Release \
        -DTH08_WEB_PROXY_GL_TO_MAIN_THREAD=OFF \
        -DTH08_WEB_OUTPUT_NAME=th08-web

docker run --rm \
    "${docker_limits[@]}" \
    --volume "${repo_root}:/src" \
    --workdir /src \
    --user "$(id -u):$(id -g)" \
    "${image}" \
    env EM_CACHE=/src/build/emscripten-cache \
    cmake --build /src/build/web-game --target th08-web --parallel 1

echo "Built ${build_dir}/th08-web.html"

# Firefox currently turns WebGL OffscreenCanvas snapshots into synchronous
# GPU readbacks. Reuse the compiled objects and relink a browser-specific build
# whose small batched WebGL command stream is proxied to a main-thread canvas.
docker run --rm \
    "${docker_limits[@]}" \
    --volume "${repo_root}:/src" \
    --workdir /src \
    --user "$(id -u):$(id -g)" \
    "${image}" \
    env EM_CACHE=/src/build/emscripten-cache \
    emcmake cmake -S /src -B /src/build/web-game -G "Unix Makefiles" \
        -DCMAKE_BUILD_TYPE=Release \
        -DTH08_WEB_PROXY_GL_TO_MAIN_THREAD=ON \
        -DTH08_WEB_OUTPUT_NAME=th08-web-firefox

docker run --rm \
    "${docker_limits[@]}" \
    --volume "${repo_root}:/src" \
    --workdir /src \
    --user "$(id -u):$(id -g)" \
    "${image}" \
    env EM_CACHE=/src/build/emscripten-cache \
    cmake --build /src/build/web-game --target th08-web --parallel 1

echo "Built ${build_dir}/th08-web-firefox.html"

# Leave the build tree configured for the primary Chromium artifact so a
# normal incremental `cmake --build` does not unexpectedly relink Firefox.
docker run --rm \
    "${docker_limits[@]}" \
    --volume "${repo_root}:/src" \
    --workdir /src \
    --user "$(id -u):$(id -g)" \
    "${image}" \
    env EM_CACHE=/src/build/emscripten-cache \
    emcmake cmake -S /src -B /src/build/web-game -G "Unix Makefiles" \
        -DCMAKE_BUILD_TYPE=Release \
        -DTH08_WEB_PROXY_GL_TO_MAIN_THREAD=OFF \
        -DTH08_WEB_OUTPUT_NAME=th08-web

cmake -E make_directory "${dist_dir}"
for artifact in \
    th08-web.html th08-web.js th08-web.wasm \
    th08-web-firefox.html th08-web-firefox.js th08-web-firefox.wasm \
    th08-web-icon.png; do
    cmake -E copy_if_different "${build_dir}/${artifact}" "${dist_dir}/${artifact}"
done
for metadata in _headers _redirects; do
    cmake -E copy_if_different \
        "${repo_root}/src/modern/web/cloudflare/${metadata}" \
        "${dist_dir}/${metadata}"
done

python3 "${repo_root}/scripts/check-web-provenance.py" --artifact "${dist_dir}"
echo "Staged ${dist_dir}"
