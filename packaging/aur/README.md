# Local AUR packaging

The split is three packages:

| Package | Contents |
|---|---|
| `wlroots0.20` | the pinned wlroots 0.20.2 library, its pkg-config file and generated protocol headers/XMLs |
| `serenity-lagom-git` | the `libgui-wayland` framework, the SerenityOS applications (with the SDE patches), resources, and the headers/source/generated headers the compositor build needs |
| `serenity-desktop-environment-git` | the compositor, services and session; built with `SDE_PREBUILT_FRAMEWORK=ON` against `serenity-lagom-git` and installed over the private `/usr/lib/sde` prefix |

The framework and the SDE still come from the same revision: the framework
package builds the SDE-patched applications, and the SDE package builds against
those exact headers (the ABI trap in `docs/26-ci.md` §6).

```sh
cd packaging/aur/serenity-desktop-environment-git
makepkg -si          # resolves the framework package; long (SerenityOS + apps)
# log out, then pick "Serenity Desktop Environment" at the login screen
```

The full package build is not run in CI: it needs network access for the
SerenityOS pin and a full framework build. What *is* tested is the split layout
(`scripts/run-split-framework-test.sh`, which stages a framework and builds the
compositor against it), the installed session (`run-install-layout-test.sh`) and
the PKGBUILD metadata (`run-packaging-test.sh`).

## Publishing

Nothing has been pushed to the AUR. The `PKGBUILD` and generated `.SRCINFO` in
each directory are ready to copy into the per-package AUR repositories, but a
real `makepkg -si` build/login must pass first and pushing needs the maintainer's
AUR account and SSH key:

```sh
# after a successful local build/login
for pkg in wlroots0.20 serenity-lagom-git serenity-desktop-environment-git; do
    (cd "$pkg" && makepkg --printsrcinfo > .SRCINFO)
    git clone "ssh://aur@aur.archlinux.org/$pkg.git" "/tmp/aur-$pkg"
    cp "$pkg/PKGBUILD" "$pkg/.SRCINFO" "/tmp/aur-$pkg/"
    (cd "/tmp/aur-$pkg" && git add PKGBUILD .SRCINFO && git commit -m "Initial import" && git push)
done
```

`.SRCINFO` is generated metadata: regenerate it whenever `pkgver()` or the
dependency/source lists change.
