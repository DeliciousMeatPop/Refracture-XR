# Refracture XR builds

Refract has no automated builds: its releases are packaged by hand (`tools/package_release.ps1`) on a PC that has
the NVIDIA, Visual Studio, JDK and dex2jar builds that package needs. Refracture XR therefore does not rebuild
Refract's runtimes. A Refracture XR package is **Refract's own release with this fork's launcher laid over it**.

## Pieces

| File | Role |
| --- | --- |
| `.github/workflows/ci.yml` | Tests and builds (Linux; Windows runs the Unknown sources tests). Manual |
| `.github/workflows/release.yml` | Builds, brands and packages; creates a draft release. Manual |
| `tools/refracture_brand.py` | Gives a throwaway copy Refracture XR's name, R mark, icons and update source |
| `tools/refracture_package.py` | Lays the launcher over Refract's release package and checks its SHA-256 |
| `branding/` | The R mark and the icon set the branding step uses |
| `tools/refracture_readme.txt` | Becomes `REFRACTURE.txt` in each package |
| `tests/test_refracture_tools.py` | Tests for both scripts |

## Branding without touching Refract's files

`tools/refracture_brand.py` runs in the release job on its own checkout, before the launcher is built. It replaces the
ring logo (`launcher/src/components/logo.jsx`), the app name in the title bar, sidebar, welcome and update dialogs and
the window title, adds an About panel with the "Based on Refract" credit, copies `branding/icons` over the Tauri icons,
and points the updater at this repository's releases (package names `Refracture-XR-<version>-<platform>`). The names of
Refract's runtimes and components ("Refract Native", "Refract components") are left as they are.

Nothing is committed back. In the repository the launcher keeps Refract's logo, name and icons, so a merge from Refract
never conflicts with branding.

To look at the branded launcher yourself, run it on a copy:

```sh
python -c "import shutil; shutil.copytree('launcher', 'dist/branded/launcher', ignore=shutil.ignore_patterns('node_modules', 'dist', 'target'))"
python tools/refracture_brand.py --root dist/branded
```

(`dist/` is not tracked by git. The copy has no `node_modules`; link or install them there to run it.)

`python tools/refracture_brand.py --check` changes nothing and reports each replacement whose Refract text is gone.
CI runs it (start CI from the Actions tab before merging from Refract), so a merge that changes one of those lines is caught. Fix it by updating the
`REPLACEMENTS` list in the script; running the script twice is harmless.

## Making a release

1. Merge Refract up to one of its **published releases** (the package it builds on must match the code in
   `launcher/backend` and `launcher/core`, which run against Refract's scripts inside the package).
2. Set the same version in `launcher/package.json` and `launcher/src-tauri/tauri.conf.json`. The updater compares
   with `package.json`, and the package name uses it.
3. Run the **Release** workflow from the Actions tab (**Run workflow**), optionally naming the Refract release tag to
   build on (empty means its latest release). Neither workflow runs on its own. To have the version checked
   against a tag, create the tag first and choose it as the ref to run on.
4. The workflow downloads Refract's Windows and Linux packages, checks each against the SHA-256 Refract's release
   publishes, builds the branded launcher, and attaches the two packages and `SHA256SUMS` to a **draft** release.
5. Try a package, then publish the draft.

A package records which Refract release it was made from, with its checksum, in `UPSTREAM.txt`.

## What is not covered

- The workflows have only been checked here as far as the scripts and YAML go. The first real run on GitHub's
  runners (the Tauri build in particular) may need small fixes.
- If Refract changes how its package is laid out (the launcher's file name, or no `launcher/core` folder), the
  packaging step stops with a message rather than producing a broken package.
- Refract is GPL-3.0-or-later with an added credit term. Redistributing its release package is allowed under that
  licence; the packages carry `LICENSE`, `NOTICE` and `CREDITS.md`, and this repository is the matching source for
  the launcher part. Refract's source for the rest is at the release tag named in `UPSTREAM.txt`.
