# uniqFEED Integration Notes

This document describes the example uniqFEED integration added to `doc/examples/transcode.c`.

The goal of the modification is to show how a transcoding pipeline can hand each filtered video frame to the uniqFEED render library, receive a modified image back, and continue normal FFmpeg encode and mux processing.

This is example code, not a finalized production integration.


## Scope

The uniqFEED changes are intentionally limited to `doc/examples/transcode.c`.

Current behavior:

- uniqFEED is compiled only when `USE_UF_RENDERLIB` is defined.
- uniqFEED is applied only to video frames.
- uniqFEED is invoked once per filtered output frame, immediately before encode.
- frame image data is exchanged with uniqFEED entirely in memory.
- the example now queries the uniqFEED context for its native resolution instead of assuming `1280x720`.
- metadata is still loaded from local `md-XXXXXX.bin` files for example purposes.

Current non-goals:

- no FFmpeg-wide filter module was added
- no permanent host-machine dependency setup for uniqFEED libraries was added to the main tree
- no content-fabric metadata loader was added here
- no `avpipe` integration was added here


## Files

Primary code location:

- `doc/examples/transcode.c`

Important uniqFEED entry points in the example:

- render context state: [doc/examples/transcode.c](doc/examples/transcode.c#L129)
- AVFrame to uniqFEED image conversion: [doc/examples/transcode.c](doc/examples/transcode.c#L137)
- example metadata loading: [doc/examples/transcode.c](doc/examples/transcode.c#L181)
- uniqFEED image back to AVFrame conversion: [doc/examples/transcode.c](doc/examples/transcode.c#L242)
- per-frame uniqFEED processing function: [doc/examples/transcode.c](doc/examples/transcode.c#L326)
- uniqFEED initialization: [doc/examples/transcode.c](doc/examples/transcode.c#L377)
- modern decode loop: [doc/examples/transcode.c](doc/examples/transcode.c#L823)
- filter/encode loop where uniqFEED is invoked: [doc/examples/transcode.c](doc/examples/transcode.c#L861)


## Build Assumptions

This example assumes the uniqFEED headers and libraries are available outside the FFmpeg tree.

The example requires:

- `USE_UF_RENDERLIB` to be defined at compile time
- include path access to `uf/renderlib/UfRenderInterface.h`
- linkage against the uniqFEED render library
- FFmpeg built with `libswscale`
- a recent uniqFEED renderlib header and runtime that provide `uFGetContextResolution()` and `uFGetContextFramerate()`

This repository does not currently wire those flags into the standard FFmpeg example build rules.

In practice, building this example requires something equivalent to:

```sh
-DUSE_UF_RENDERLIB
-I/path/to/tnt-uniqfeed/include
-L/path/to/uniqfeed/libs
-luf-renderlib
```

The exact library search paths depend on the local uniqFEED runtime layout.


## Recommended Container Workflow

The most reliable way to work with this example is to build it inside the uniqFEED Docker environment instead of installing uniqFEED runtime requirements on the host.

This branch now includes:

- [.env](.env)
- [docker/uniqfeed-ffmpeg/Dockerfile](docker/uniqfeed-ffmpeg/Dockerfile)
- [docker-compose.uniqfeed.yml](docker-compose.uniqfeed.yml)
- [tools/build-uniqfeed-example.sh](tools/build-uniqfeed-example.sh)
- [tools/run-uniqfeed-example.sh](tools/run-uniqfeed-example.sh)
- [tools/uniqfeed-container.sh](tools/uniqfeed-container.sh)

The primary workflow uses the existing uniqFEED runtime image directly through [docker-compose.uniqfeed.yml](docker-compose.uniqfeed.yml).

These files assume you already have the uniqFEED container image available locally.

On this machine, the discovered local tag is:

```sh
uf_render_interface:ubuntu_22
```

That default is now stored in the repo-root [.env](.env) file.

The wrapper script reads `.env` automatically. If your local image uses a different tag, either edit `.env` or override `UF_BASE_IMAGE` on the command line.

In the sibling uniqFEED repository, the expected base image is produced by its own Docker setup. The directly matching commands are:

```sh
cd /path/to/tnt-uniqfeed
docker compose build uf_render_interface
```

or equivalently:

```sh
cd /path/to/tnt-uniqfeed
docker build -t uf_render_interface:ubuntu_22 .
```

If you want the FFmpeg container workflow to use a sibling checkout such as `/path/to/tnt-uniqfeed` instead of the runtime bundled at `/runtime`, set `UF_RUNTIME_ROOT` to that absolute path before invoking the wrapper script. The script will bind-mount that path into the container and build/run against its `include` and `lib` directories.

If your uniqFEED runtime depends on additional shared-library directories outside that tree, set `UF_RUNTIME_EXTRA_LIB_DIRS` to a colon-separated list of directories and the helper scripts will append them to `LD_LIBRARY_PATH` during configure, build, and run.

The container wrapper expects the uniqFEED base image to already exist locally. This FFmpeg repository does not build that base image; it only runs against the tag configured by `UF_BASE_IMAGE`.

The container-based workflow avoids polluting the host with pinned uniqFEED dependencies such as gRPC, Vulkan runtime pieces, Boost 1.74, hiredis 0.14, and other bundled libraries.


## What Runs Where

There are two layers involved in this workflow:

1. the uniqFEED base image, built from the uniqFEED repository
2. the FFmpeg build and run commands, invoked from this FFmpeg repository

What must already exist before using this FFmpeg branch:

- the uniqFEED Docker image referenced in [.env](.env)

On this machine, that is:

```sh
uf_render_interface:ubuntu_22
```

That image is expected to be built from the uniqFEED repository's own Docker setup.

What you run from the host in this FFmpeg repository:

```sh
tools/uniqfeed-container.sh build
tools/uniqfeed-container.sh run input.mp4 output.mp4 /runtime/project /runtime/example_input
tools/uniqfeed-container.sh shell
```

Those commands are started on the host, but they execute inside the uniqFEED container automatically through Docker Compose.

So you do not manually run the FFmpeg build steps inside a pre-opened shell unless you want to debug interactively, and you do not need a separate `docker compose up` step in this FFmpeg repository.


## Container Build Steps

Run the FFmpeg build helper inside the uniqFEED container:

```sh
docker compose -f docker-compose.uniqfeed.yml run --rm ffmpeg-uniqfeed \
  tools/build-uniqfeed-example.sh
```

Or use the wrapper script from the FFmpeg repo root:

```sh
tools/uniqfeed-container.sh build
```

Or, to override the uniqFEED base image tag:

```sh
UF_BASE_IMAGE=uf_render_interface:ubuntu_22 \
tools/uniqfeed-container.sh build
```

That script installs the FFmpeg build tools it needs inside the disposable container, configures FFmpeg against the uniqFEED runtime bundled into the base image at:

- `/runtime/include`
- `/runtime/lib`
- `/runtime/lib/uf`
- `/runtime/lib/3rdparty`

and then builds:

- `doc/examples/transcode`

The helper keeps repeated rebuilds incremental. It only drops existing `*.o` and `*.d` files when the uniqFEED-related build configuration changes, which preserves the stale-dependency workaround without forcing a full rebuild on every invocation.

If you need to force a cold rebuild anyway, pass `--clean` to the helper or wrapper:

```sh
tools/build-uniqfeed-example.sh --clean
tools/uniqfeed-container.sh build --clean
```

If you need extra FFmpeg configure flags, append them after the script name:

```sh
docker compose -f docker-compose.uniqfeed.yml run --rm ffmpeg-uniqfeed \
  tools/build-uniqfeed-example.sh --enable-gpl --enable-shared
```

With the wrapper script:

```sh
tools/uniqfeed-container.sh build --enable-gpl --enable-shared
```

If you want an interactive shell inside the same container environment, run:

```sh
docker compose -f docker-compose.uniqfeed.yml run --rm ffmpeg-uniqfeed
```


## Container Run Steps

After building the example, run it through the helper script so execution happens inside the same uniqFEED runtime environment:

```sh
docker compose -f docker-compose.uniqfeed.yml run --rm ffmpeg-uniqfeed \
  tools/run-uniqfeed-example.sh input.mp4 output.mp4 /runtime/project /path/to/metadata_dir
```

With the wrapper script:

```sh
tools/uniqfeed-container.sh run input.mp4 output.mp4 /runtime/project /path/to/metadata_dir
```

The helper script ensures the uniqFEED shared-library paths are present before launching [doc/examples/transcode](doc/examples/transcode).

To open an interactive shell in the same container environment:

```sh
tools/uniqfeed-container.sh shell
```


## Build Instructions

There are two practical ways to build this example:

1. build FFmpeg in-tree and let the FFmpeg build system build `doc/examples/transcode`
2. build the examples with the example Makefile approach described in `doc/examples/README`

For this uniqFEED variant, the in-tree build is the most straightforward option.


## Recommended In-Tree Build

The stock FFmpeg examples README says to build FFmpeg first and then run `make examples`. That guidance is in [doc/examples/README](doc/examples/README).

For the uniqFEED-enabled `transcode` example, configure FFmpeg with the uniqFEED include path, library path, and compile definition added globally.

A typical flow looks like this:

```sh
./configure \
  --extra-cflags="-DUSE_UF_RENDERLIB -I/path/to/tnt-uniqfeed/include" \
  --extra-ldflags="-L/path/to/uniqfeed/lib -L/path/to/uniqfeed/lib/uf -L/path/to/uniqfeed/lib/3rdparty" \
  --extra-libs="-luf-renderlib" \
  [your normal FFmpeg configure flags]

make -j$(nproc)
make doc/examples/transcode
```

Or, if you want all enabled examples:

```sh
make examples
```

Notes:

<<<<<<< HEAD
- `transcoding` is an FFmpeg example target listed in [doc/examples/Makefile](doc/examples/Makefile#L21).
- the main tree includes the examples makefile from [Makefile](Makefile#L98).
- the `transcoding` example depends on FFmpeg filter/codec/format/util libraries, as declared in `configure`
=======
- `transcode` is an FFmpeg example target listed in [doc/examples/Makefile](doc/examples/Makefile#L21).
- the main tree includes the examples makefile from [Makefile](Makefile#L98).
- the `transcode` example depends on FFmpeg filter/codec/format/util libraries, as declared in `configure`
>>>>>>> 74091b9753 (rebase on main, most runtime to libavfilter)
- this README uses placeholder library names because the exact uniqFEED artifact names are local-build specific


## Example Build Command

If the uniqFEED project is located at `/path/to/tnt-uniqfeed`, the command will likely look conceptually like this:

```sh
./configure \
  --extra-cflags="-DUSE_UF_RENDERLIB -I/path/to/tnt-uniqfeed/include" \
  --extra-ldflags="-L/path/to/tnt-uniqfeed/lib -L/path/to/tnt-uniqfeed/lib/uf -L/path/to/tnt-uniqfeed/lib/3rdparty" \
  --extra-libs="-luf-renderlib" \
  [your existing FFmpeg options]

make -j$(nproc)
make doc/examples/transcode
```


## Alternative Example-Build Flow

The stock FFmpeg examples README also describes building examples separately with `pkg-config` from `doc/examples/README`.

That flow is useful for plain FFmpeg examples, but for this uniqFEED variant you still need to inject:

- `-DUSE_UF_RENDERLIB`
- the uniqFEED include directory
- the uniqFEED library directory
- the uniqFEED library name

Because of that, the in-tree build is usually simpler and less error-prone for this modified example.


## Output Location

When built in-tree, the resulting example binary is generated at:

```text
doc/examples/transcode
```

That target is produced by the examples build rules in [doc/examples/Makefile](doc/examples/Makefile).


## Common Build Issues

- `uf/renderlib/UfRenderInterface.h: No such file or directory`
  The uniqFEED include path was not added to `--extra-cflags`.

- `undefined reference` errors for uniqFEED symbols or transitive uniqFEED libraries
  The uniqFEED library paths were not added correctly to link flags, or the build is running outside the curated uniqFEED runtime environment.

- `USE_UF_RENDERLIB` code not compiled in
  The `-DUSE_UF_RENDERLIB` define was not passed in through `--extra-cflags`.

- `doc/examples/transcode` not built by `make examples`
  The example target may not be enabled in your configure result, or the tree may need a full rebuild after reconfiguring.

- linker errors for gRPC, Boost, Vulkan, hiredis, or other uniqFEED dependencies on the host
  Build inside the Docker workflow above instead of trying to reproduce the uniqFEED runtime stack directly on the host.


## Runtime Usage

When built with uniqFEED enabled, the example usage is:

```sh
./transcode input output project_path [metadata_dir]
```

Optional runtime behavior can be enabled with:

```sh
UF_RENDERLIB_PASSTHROUGH_ON_FAILURE=1
```

Arguments:

1. `input`
2. `output`
3. `project_path`
4. `metadata_dir` (optional when an external metadata provider is linked)

<<<<<<< HEAD
The usage string is defined in [doc/examples/transcoding.c](doc/examples/transcoding.c#L958).
=======
The usage string is defined in [doc/examples/transcode.c](doc/examples/transcode.c#L958).
>>>>>>> 74091b9753 (rebase on main, most runtime to libavfilter)

With `UF_RENDERLIB_PASSTHROUGH_ON_FAILURE=1`, the example treats recoverable uniqFEED errors as a signal to disable uniqFEED for the rest of the run and continue encoding the original filtered frames.

The current integration enforces a frame-size guard before calling uniqFEED: the filtered video frame must match the active uniqFEED project context resolution returned by `uFGetContextResolution()`. Any other size is rejected before metadata lookup or renderer invocation.

Example with the container wrapper:

```sh
UF_RENDERLIB_PASSTHROUGH_ON_FAILURE=1 \
tools/uniqfeed-container.sh run input.mp4 output.mp4 /runtime/project /path/to/metadata_dir
```


## Metadata Convention

For example purposes, metadata is loaded from files named:

```text
md-000000.bin
md-000001.bin
md-000002.bin
...
```

These files are resolved relative to the supplied `metadata_dir`.

The current code uses a per-stream monotonically increasing frame counter, not packet PTS lookup, to choose the metadata file.

If a metadata file is missing, the example falls back to `uFCreateMetadata(NULL, 0)`, which means uniqFEED receives an empty/default metadata object.

The newer uniqFEED drop also ships structured project metadata JSON files (for example `project/project.json`, `project/scenes.json`, `project/media_composer.json`, and camera-specific JSON files under `project/camera_<uuid>/`). Those JSON files describe static scene/layout data such as zones, clip surfaces, and board geometry. This FFmpeg example does not parse those JSON files directly; it passes `project_path` into `uFCreateContext()`, and uniqFEED loads project data internally.

Per-frame dynamic overlay metadata for this example can now come from either source:

- an external metadata provider callback (recommended path for avpipe integration)
- `metadata_dir` with `md-XXXXXX.bin` files (legacy/sample fallback path)

This is intentionally simple and is expected to be replaced later by a metadata provider more suitable for production, such as content-fabric object metadata passed in through another consumer.


## avpipe Integration Mechanism

The uniqFEED runtime now exposes an optional external metadata-provider ABI in [libavfilter/uf_render_metadata_provider.h](libavfilter/uf_render_metadata_provider.h).

If a linked object (for example from `~/src/avpipe`) defines `uFGetExternalMetadataProviderV1()`, `doc/examples/transcode` will call that provider for per-frame metadata instead of relying only on local `md-XXXXXX.bin` files.

Provider lifecycle:

1. `init(project_path, metadata_dir, provider_opaque)` runs once during uniqFEED initialization.
2. `get_metadata_blob(frame_index, stream_index, render_tid, filtered_frame, ...)` runs once per filtered video frame.
3. `release_metadata_blob(...)` is called after each frame metadata blob is consumed.
4. `close(provider_opaque)` runs during teardown.

If the provider is present but returns an error for a frame, the current logic optionally falls back to file metadata when `metadata_dir` is set.

This gives avpipe a clean insertion point to map its own timeline/object metadata into uniqFEED without changing the core frame-processing path.


## High-Level Architecture

The modified transcoding pipeline is:

```text
demux
  -> decode
  -> FFmpeg filter graph (includes uniqfeed filter)
  -> encode
  -> mux
```

The important design decision is that uniqFEED runs as a regular `libavfilter` video filter.

That keeps runtime processing reusable for both `transcode` and other callers (for example, avpipe) that can build an FFmpeg filter graph.


## Control Flow

At a high level, the frame flow is:

1. `decode_filter_encode_write_frame()` submits packets to the decoder and receives decoded frames.
2. `init_filters()` builds a filter chain that includes `uniqfeed=...` for video streams.
3. Each decoded frame is pushed into that filter graph.
4. `filter_encode_write_frame()` pulls already-processed frames from the filter graph.
5. The filtered frame is encoded and then muxed.

<<<<<<< HEAD
The decode path is in [doc/examples/transcoding.c](doc/examples/transcoding.c#L823), and the uniqFEED hook is in [doc/examples/transcoding.c](doc/examples/transcoding.c#L901).
=======
The graph construction is in [doc/examples/transcode.c](doc/examples/transcode.c), and the runtime filter implementation is in [libavfilter/vf_uniqfeed.c](libavfilter/vf_uniqfeed.c).
>>>>>>> 74091b9753 (rebase on main, most runtime to libavfilter)


## In-Memory Image Exchange

The uniqFEED filter performs in-memory conversion:

- Converts the incoming `AVFrame` into an RGB `UfImage`.
- Renders with `uFRenderFeeds(...)`.
- Converts the selected output feed image back into an `AVFrame`.

<<<<<<< HEAD
Instead:

- `create_render_image_from_frame()` converts the FFmpeg `AVFrame` into RGB24 and writes the pixels directly into a `UfImage` host buffer.
- uniqFEED renders against that in-memory image.
- `create_frame_from_render_image()` converts the returned uniqFEED image back into an `AVFrame` using `libswscale`.

This reduces overhead and keeps the image handoff local to process memory.

The conversion helpers are located at:

- [doc/examples/transcoding.c](doc/examples/transcoding.c#L137)
- [doc/examples/transcoding.c](doc/examples/transcoding.c#L242)
=======
The conversion helpers and render path are in [libavfilter/vf_uniqfeed.c](libavfilter/vf_uniqfeed.c).
>>>>>>> 74091b9753 (rebase on main, most runtime to libavfilter)


## uniqFEED-Specific Flow

For each frame entering the `uniqfeed` filter:

1. Create a `UfImage` in `R8G8B8_UINT` format.
2. Convert the FFmpeg frame into RGB24 and populate the uniqFEED host buffer.
3. Load metadata either from the external provider ABI or from `md-XXXXXX.bin` files.
4. Call `uFRenderFeeds(ctx, metadata, tid, input_image)`.
5. If uniqFEED returns no feeds yet, pass the original frame through unchanged.
6. Retrieve feed index `0`.
7. Convert the returned image back into an FFmpeg frame with the original timing properties.
8. Forward the resulting frame to downstream filters/encoder.

<<<<<<< HEAD
The main uniqFEED work happens in [doc/examples/transcoding.c](doc/examples/transcoding.c#L326).
=======
The main uniqFEED work happens in [libavfilter/vf_uniqfeed.c](libavfilter/vf_uniqfeed.c).
>>>>>>> 74091b9753 (rebase on main, most runtime to libavfilter)


## Why Runtime Is In `libavfilter`

uniqFEED runtime logic moved out of `doc/examples/transcode.c` and into `libavfilter/vf_uniqfeed.c` so it is reusable and composable.

That matters because:

- `transcode` remains glue code that builds a filtergraph.
- avpipe (and other integrations) can use the same runtime path without copying logic.
- uniqFEED stays in the standard FFmpeg filter lifecycle (`init`, frame processing, `uninit`).

<<<<<<< HEAD
The hook site is [doc/examples/transcoding.c](doc/examples/transcoding.c#L861).
=======
The filtergraph hook is in [doc/examples/transcode.c](doc/examples/transcode.c), and the runtime implementation is in [libavfilter/vf_uniqfeed.c](libavfilter/vf_uniqfeed.c).
>>>>>>> 74091b9753 (rebase on main, most runtime to libavfilter)


## Current Limitations

- Only video streams are sent through uniqFEED.
- Only feed index `0` is used from the returned `UfFeeds` set.
- Metadata selection is frame-counter-based.
- The provider ABI is optional; without a provider, metadata falls back to `metadata_dir` files.
- The example assumes uniqFEED accepts RGB input and returns an image that can be converted back to the source frame format.
- The current bundled uniqFEED project is guarded to only run on filtered video frames that match the uniqFEED context resolution.
- `UF_RENDERLIB_PASSTHROUGH_ON_FAILURE=1` only helps for recoverable errors returned through the uniqFEED API. It cannot recover if the uniqFEED library aborts the process internally, such as the observed `Matting.cpp` fail-fast path.


## Likely Next Steps

If this example evolves toward production use, the most likely next steps are:

1. Replace `load_render_metadata()` with a metadata-provider abstraction.
2. Feed metadata from content-fabric or `avpipe` instead of local files.
3. Decide whether feed selection should always be index `0` or be configurable.
4. Optionally move the behavior into a dedicated FFmpeg filter if the integration is meant to become a reusable pipeline stage.


## Summary

The uniqFEED changes in `doc/examples/transcode.c` demonstrate a minimal but realistic frame-processing pattern:

- decode with modern send/receive APIs
- run normal FFmpeg filters
- hand each filtered video frame to uniqFEED in memory
- receive the modified image back
- encode and mux as usual

That keeps the example small while matching the intended long-term direction, where frame processing remains the same but metadata delivery is supplied by a different upstream consumer.