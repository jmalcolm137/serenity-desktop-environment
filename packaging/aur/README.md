# Local AUR packaging

The first SDE package is deliberately **one** package: it builds the SDE and the
`libgui-wayland` framework from the same rev, so the compositor, services and
apps share one set of headers (the ABI trap in `docs/26-ci.md` §6). SerenityOS is
fetched by `libgui-wayland/scripts/fetch-serenity.sh` during `build()`, and
wlroots is vendored by the SDE build; both are installed under the private
`/usr/lib/sde` prefix by `scripts/install-sde.sh`.

`wlroots0.20/` is the first split package: it builds the pinned 0.20.2 release and
installs its library, pkg-config file, generated protocol headers and protocol
XMLs. The single SDE package still vendors wlroots; the split SDE package will
use this one once the framework exposure problem is solved.

```sh
cd packaging/aur/serenity-desktop-environment-git
makepkg -si          # long: the framework and the application suite
# log out, then pick "Serenity Desktop Environment" at the login screen
```

The package is not built in CI: it needs network access for the SerenityOS pin
and a full framework build, and a package build is not a test. What *is* tested
is the layout it installs (`scripts/run-install-layout-test.sh`) and the session
it starts.

Splitting this into `wlroots0.20`, `serenity-lagom-git` and
`serenity-desktop-environment-git` (the plan in
`docs/13-native-session-and-aur-packaging.md` §3) is the follow-up once this one
has been installed on a real machine.
