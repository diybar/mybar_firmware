# MyBar Firmware CI/CD

The firmware is built, released and published automatically by GitHub Actions. This document explains what runs when, how to cut a release, how to build the same way locally, and what to do when something fails.

Related documents:

* [FIRMWARE_UPDATE.md](FIRMWARE_UPDATE.md) — over-the-air update protocol used by the mobile app
* [WEB_FLASHER.md](WEB_FLASHER.md) — the browser based USB flasher deployed by this pipeline

---

## 1. Overview

```
  pull request / push              push to main
  ───────────────────              ────────────────────────────────────────────────
  Compile ──► artifact             Compile ──► artifact
                                      │
                                      ├──► GitHub release   tag v<version> + binaries
                                      │                     (skipped if the tag exists)
                                      │
                                      └──► Deploy web flasher   GitHub Pages
                                                                https://diybar.github.io/mybar_firmware/
```

Everything lives in one workflow file, [`.github/workflows/firmware.yml`](../.github/workflows/firmware.yml), and one build script, [`scripts/build.sh`](../scripts/build.sh).

| Trigger | Compile | GitHub release | Deploy web flasher |
|---|---|---|---|
| Pull request | yes | no | no |
| Push to `main` (merge) | yes | yes, if `v<version>` does not exist yet | yes |
| Manual run (*Actions → Firmware → Run workflow*) on `main` | yes | same as push | yes |
| Manual run on another branch | yes | no | no |

The version is read from the sketch:

```cpp
const String firmwareVersion = "1.96";
```

The tag, release title, file names (`mybar-1.96.bin`) and the version shown on the flasher page are all derived from this one constant.

---

## 2. Jobs in detail

### 2.1 Compile

Runs on `ubuntu-latest`.

1. Installs `arduino-cli` (`ARDUINO_CLI_VERSION`, currently 1.5.1) with `arduino/setup-arduino-cli`.
2. Restores `~/.arduino15` from the Actions cache (key includes the core version) and installs the **esp32 core** pinned in `ESP32_CORE_VERSION` (currently **3.3.5**). On a cache hit this step is a no-op and the job takes about 2.5 minutes; on a miss it downloads roughly 1 GB of toolchain and takes a few minutes longer.
3. Runs `scripts/build.sh` (see section 4).
4. Writes a summary table (file, size, MD5) to the run's *Summary* tab and uploads `dist/` as the artifact **`mybar-firmware-<version>`** (kept for 30 days).

The `version`, `size` and `md5` values are exposed as job outputs for the other two jobs.

### 2.2 GitHub release

Runs only for pushes to `main`. Needs `contents: write`, which the default `GITHUB_TOKEN` provides; no secrets are required.

1. Downloads the artifact from the Compile job.
2. Checks whether the tag `v<version>` already exists on the remote.
   * **Exists:** prints a notice and stops. Nothing is published. This is what happens when a change is merged without bumping `firmwareVersion`.
   * **Does not exist:** continues.
3. Runs `gh release create v<version>` targeting the merged commit. Git creates the tag as part of this call, so no tag has to be pushed by hand. The release contains:

| Asset | Purpose |
|---|---|
| `mybar-<v>.bin` | OTA image sent by the app |
| `mybar-<v>.merged.bin` | Complete 4 MB flash image, flash at `0x0` with esptool |
| `mybar-<v>.bootloader.bin` | Bootloader, offset `0x1000` |
| `mybar-<v>.partitions.bin` | Partition table, offset `0x8000` |
| `boot_app0.bin` | OTA data initialiser, offset `0xE000` |
| `SHA256SUMS`, `MD5SUMS` | Checksums of every `.bin` |
| `version.json` | Version, OTA size, MD5, commit, build time (see 2.4) |
| `mybar-<v>.elf`, `mybar-<v>.map` | Debug symbols for crash decoding |

The release notes are generated from a template in the workflow and include the exact `otaStart:<size>:<md5>` command, a link to the flasher and an esptool one-liner, followed by GitHub's auto generated list of merged pull requests.

### 2.3 Deploy web flasher

Runs only for pushes to `main`, in parallel with the release job. Needs `pages: write` and `id-token: write`, again provided by `GITHUB_TOKEN`.

1. Downloads the artifact.
2. Assembles the site: everything in `web/`, plus `manifest.json`, `version.json` and the `firmware/` folder from the build.
3. Publishes it with `actions/deploy-pages`. The result is available at `https://diybar.github.io/mybar_firmware/` a minute or so after the job finishes.

The site always serves the **latest build of `main`**, even when the release step was skipped. Older versions remain available on the releases page.

`actions/configure-pages` runs with `enablement: true`, so the first deploy creates the Pages site (source: GitHub Actions) by itself. If the token is not allowed to do that, create it by hand under *Settings → Pages → Build and deployment → Source: **GitHub Actions*** (or `gh api -X POST repos/diybar/mybar_firmware/pages -f build_type=workflow`) and re-run the failed job.

### 2.4 `version.json`

Published both as a release asset and at `https://diybar.github.io/mybar_firmware/version.json`. The mobile app can fetch it to discover the current firmware and build the `otaStart` command:

```json
{
  "version": "1.96",
  "bin": "firmware/mybar-1.96.bin",
  "size": 1121248,
  "md5": "ade0b8525105078ee6bc645b97bd7365",
  "mergedBin": "firmware/mybar-1.96.merged.bin",
  "commit": "b8c7de6…",
  "builtAt": "2026-09-03T03:17:23Z"
}
```

`bin` and `mergedBin` are relative to the Pages site root, so `https://diybar.github.io/mybar_firmware/` + `bin` is the download URL of the OTA image.

---

## 3. Releasing a new firmware version

1. Make your changes on a branch.
2. Bump `firmwareVersion` in `src/MyBar_*.ino`. Use the same `major.minor` style as before (`"1.97"`); the tag becomes `v1.97`.
3. Open a pull request. Wait for **Compile** to pass. If the change touches BLE, OTA or the partition layout, download the artifact from the run and test it on a board first (flash `merged.bin` with esptool or send `mybar-<v>.bin` through the app).
4. Merge. Within a few minutes:
   * the release `v1.97` appears under *Releases*,
   * the flasher at `https://diybar.github.io/mybar_firmware/` shows *Installs firmware v1.97*.
5. Update the app, or let it read `version.json`, so it sends `otaStart:<size>:<md5>` with the new values.

Rules of thumb:

* **One version per merge.** If two PRs both bump to 1.97 the second merge is skipped as "tag exists". Rebase and bump again.
* **Never reuse a version.** Tags are immutable; if a release is broken, bump to 1.98 and merge a fix. Delete the bad release and tag from GitHub if you want to hide it.
* **Docs-only merges are fine.** They rebuild and redeploy the flasher with the unchanged version and skip the release.

### Re-running a failed release

If the Compile job succeeded but the release or Pages job failed (network hiccup, Pages not enabled yet), fix the cause and use *Re-run failed jobs* on the run. The artifact is reused, nothing is recompiled.

---

## 4. The build script

`scripts/build.sh` is the single source of truth for how the firmware is built. CI calls it unchanged, so a local run reproduces the CI build.

What it does:

1. Finds the one `.ino` in `src/` and reads `firmwareVersion` from it.
2. Copies `src/` into a temporary folder named after the sketch (`MyBar_1.96/`). Arduino requires the folder and the `.ino` to share a name, which is why the sources cannot be compiled in place from `src/`.
3. Compiles with

   ```
   arduino-cli compile --fqbn esp32:esp32:esp32:PartitionScheme=default --libraries Libraries/library --output-dir <tmp> <sketch>
   ```

   `PartitionScheme=default` is *Default 4MB with spiffs*, which has the two app slots OTA needs. `--libraries Libraries/library` makes `Streaming`, `QueueList` and `L9110 Motor driver` visible without installing them.
4. Locates `boot_app0.bin` inside the installed core (`runtime.platform.path` from `arduino-cli compile --show-properties`).
5. Writes `dist/`:

   ```
   dist/
   ├── manifest.json          ESP Web Tools manifest (paths relative to this folder)
   ├── version.json
   ├── firmware/
   │   ├── mybar-<v>.bin
   │   ├── mybar-<v>.merged.bin
   │   ├── mybar-<v>.bootloader.bin
   │   ├── mybar-<v>.partitions.bin
   │   ├── boot_app0.bin
   │   ├── SHA256SUMS
   │   └── MD5SUMS
   └── debug/
       ├── mybar-<v>.elf
       └── mybar-<v>.map
   ```

Environment variables:

| Variable | Default | Purpose |
|---|---|---|
| `OUT` | `<repo>/dist` | Output directory (deleted and recreated) |
| `FQBN` | `esp32:esp32:esp32:PartitionScheme=default` | Board and options. Use `...:PartitionScheme=min_spiffs` if the sketch outgrows 1,310,720 bytes |

### Building locally

```
arduino-cli config init
arduino-cli config add board_manager.additional_urls https://espressif.github.io/arduino-esp32/package_esp32_index.json
arduino-cli core update-index
arduino-cli core install esp32:esp32@3.3.5
bash scripts/build.sh
```

Works on Linux, macOS and Windows (Git Bash). A full build takes 3 to 5 minutes.

### Changing the core version

Edit `ESP32_CORE_VERSION` in the workflow **and** the instructions above (README and this file). Install the same version locally so local and CI binaries match. Core 3.x is required because the sketch uses `ledcAttach`.

---

## 5. Troubleshooting

| Symptom | Cause | Fix |
|---|---|---|
| `Process completed with exit code 126` in *Build firmware* | `scripts/build.sh` lost its executable bit (commits from Windows) | The workflow calls `bash scripts/build.sh`, so this should not recur. If it does: `git update-index --chmod=+x scripts/build.sh` |
| `error: expected exactly one .ino in src/` | A second `.ino` was added or the sketch was renamed away | Keep exactly one sketch in `src/` |
| `error: could not read firmwareVersion` | The constant was renamed or reformatted | Keep the line `const String firmwareVersion = "x.yz";` |
| `Sketch too big` | Image over 1,310,720 bytes | Reduce the sketch or set `FQBN` to `PartitionScheme=min_spiffs` (also update `docs/FIRMWARE_UPDATE.md`, the OTA slot size changes) |
| Release job: *Tag vX already exists* notice | Version not bumped | Expected; bump `firmwareVersion` if a release was intended |
| Release job: `HTTP 403` from `gh release create` | Workflow permissions restricted to read only | *Settings → Actions → General → Workflow permissions → Read and write*, or keep read only and rely on the job level `contents: write` (already set) |
| Pages job: *Get Pages site failed* / *Not Found* | Pages site does not exist and automatic enablement failed | *Settings → Pages → Source: GitHub Actions* (or `gh api -X POST repos/<owner>/<repo>/pages -f build_type=workflow`), then *Re-run failed jobs*. Happened once on 2026-09-03 before `enablement: true` was added. |
| Pages job passes but the site shows the old version | Browser cache | The page fetches `version.json` with `cache: no-cache`; hard-reload (`Ctrl+F5`) once |
| Core install is slow every run | Cache miss | Check the cache key still matches `ESP32_CORE_VERSION`; caches are evicted after 7 days without use |
| Annotation: *Node.js 20 is deprecated* | Upstream actions still target Node 20 | Harmless warning; bump the action majors when new ones are published |

Logs: *Actions → Firmware → (run) → Compile → Build firmware*. The `Sketch uses N bytes` line and the `OTA image:` line are the ones to check.

---

## 6. Security and permissions

* The workflow's default token permission is `contents: read`. Only the release job elevates to `contents: write` and only the Pages job gets `pages: write` and `id-token: write`.
* No repository secrets are used. Nothing needs rotating.
* Pull requests from forks run the Compile job with a read only token; they never release or deploy.
* Third party actions are pinned to major versions (`@v4`, `@v2`). Pin to commit SHAs if the repository's policy requires it.
