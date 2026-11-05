# mpc-addin-sample-rack

An addin for the MPC Sample that manages the plugins in a project's inserts. The Sample has no screen for adding or
removing a plugin, and it can't load or unload one in a running project, so this addin edits the project for it.

![The RACK page on the MPC Sample: the dial moves through the inserts, a press opens the plugin's own page](docs/screenshots/rack-page.gif)

In mpc-addin-commander's browser UI (the same rack, through `src/bridge.c`), with the plugins' panels beside it:

![Commander's rack panel on the MPC Sample](docs/screenshots/commander-rack.png)
![Pending edits in Commander, before APPLY restarts MPC](docs/screenshots/commander-rack-pending.png)
![Stompbox in Commander on the MPC Sample](docs/screenshots/commander-stompbox.png)
![Keyscope in Commander on the MPC Sample](docs/screenshots/commander-keyscope.png)

- **RACK page** (needs mpc-addin-sample-draw-handler 0.3 or later): a long hold of PAD FX opens it. It lists the
  plugins on the input, the outputs, the submixes and the tracks, and lets you add a plugin, turn one on or off, or
  remove one. A dial press on a plugin opens that plugin's own page, if it has one (mpc-vst-plugins' `"sample_page"`).
- **APPLY restarts MPC.** The edits are saved, then `sample-rack-apply` runs as a systemd unit of its own: it stops
  MPC, edits the project's working copy (`/data/TempProjects/Temp-Project.xpj`) and starts MPC again. Save the project
  first. Everything else in the file stays byte-for-byte the same.
- **Limits:** one of each plugin per project, four inserts per channel. MPC's own effects are shown but never changed.
- **For other addins in the same process** (`src/bridge.c`): `mpc_sample_rack_api()` (the interface version, 1),
  `mpc_sample_rack_state()` (the rack as a JSON document, malloc'd) and `mpc_sample_rack_apply(json, why, n)` (the same
  apply as the page, refused while one is running). mpc-addin-commander finds them with `dlsym` and offers the rack in
  its browser UI. There is no socket.

## Settings

`mpc_sample_rack.conf`, next to the library: `enabled` (0 leaves it loaded but idle), `project`, `settings` (the plugins
MPC knows) and `service` (the one APPLY restarts, `ac50.service`).

## Install

With the shared addin installer from mpc-vst-plugins (`tools/release/addin`): `tools/release-aarch64.sh <version>`
builds the zip; on the device, `sh install.sh` in the unpacked folder.

## Develop

`build-aarch64.sh` builds `build/aarch64/mpc_sample_rack.so` and `sample-rack-apply` in an arm64 container.
`tests/test.sh` runs the host tests under ASan and UBSan: the core reads a project and the plugin list and saves edits
(`core_test`), the exports for other addins (`bridge_test`), and `sample-rack-apply` on a copy of a project.

## License

MIT.
