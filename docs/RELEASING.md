# Releasing FennecTV

One script does it: `packaging/release`. It checks, builds the Linux
AppImage and the Windows zip in Docker (nothing on the host), packs the
source, writes `SHA256SUMS` and the notes, and with `--publish` tags the
commit and makes the GitHub release.

## Once, before the first release

```
sudo pacman -S github-cli      # gh
gh auth login                  # GitHub.com, SSH, a browser login is fine
```

Docker is there already (the packages build in it). The first build of
each image takes a while (VVenC, VVdeC, FFmpeg are compiled); after that
Docker's cache makes it minutes.

## Every release (0.1.0, "First public beta", as the example)

1. **The version.** `VERSION` holds `0.1.0`.

2. **The notes.** `CHANGELOG.md` has a section headed with the version
   and, if you like, a name:

   ```
   ## 0.1.0 - First public beta
   ```

   Everything under it, to the next `## `, is the release's text on
   GitHub; the script adds the downloads, how to check them and how to
   run them (the AppImage's `tx`/`rx` and `--install`, the Windows zip:
   `packaging/release-notes/how-to-run.md`, the same in every release). The name
   becomes the title: "FennecTV 0.1.0 - First public beta". `alpha`,
   `beta` or `rc` in the name or version makes it a pre-release (marked
   so on GitHub, not offered as "Latest").

3. **Commit and push** everything (the script refuses uncommitted work,
   and the tag must point at a commit GitHub has):

   ```
   git add -A
   git commit -m "FennecTV 0.1.0 - First public beta"
   git push
   ```

4. **Check** (seconds; nothing built, nothing sent):

   ```
   packaging/release --check
   ```

   Each line ✓ or ✗: the changelog section, nothing uncommitted, gh
   logged in, the tag `v0.1.0` free, HEAD on GitHub.

5. **Build** (minutes):

   ```
   packaging/release
   ```

   Into `dist/release/0.1.0/`:

   | File | |
   |---|---|
   | `FennecTV-0.1.0-x86_64.AppImage` | Linux |
   | `fennectv-0.1.0-win64.zip` | Windows |
   | `fennectv-0.1.0-src.tar.gz` | the source of the commit |
   | `SHA256SUMS` | their checksums |
   | `NOTES.md` | the release text, as GitHub will show it |
   | `COMMIT` | the commit it was built from |

   Look before publishing: read `NOTES.md`; run the AppImage
   (`./FennecTV-0.1.0-x86_64.AppImage tx --version`); if you can, the
   zip on a Windows machine. Add `--media` to run the media test too.

6. **Publish:**

   ```
   packaging/release --publish
   ```

   It checks again, reuses the build from step 5 (same commit, checksums
   still right; otherwise it builds again), then:

   - tags the commit `v0.1.0` ("FennecTV 0.1.0 - First public beta") and
     pushes the tag;
   - creates the GitHub release with the four files and the notes, as a
     pre-release here;
   - prints its address.

   `--publish --draft` makes it a draft instead: you read it on GitHub
   and press Publish there.

## When something goes wrong

- **A ✗ in the check:** it says what to do (commit, `gh auth login`, push,
  bump `VERSION`).
- **The build fails:** nothing is tagged or sent; fix, commit, push, run
  again.
- **After publishing, a mistake in the text:** edit the release on GitHub
  (or `gh release edit v0.1.0 --notes-file ...`).
- **A broken file in a published release:** don't replace files under the
  same tag; delete the release and the tag
  (`gh release delete v0.1.0 --cleanup-tag`) only if nobody can have
  downloaded it yet, otherwise release 0.1.1.

## Version numbers

`MAJOR.MINOR.PATCH`. While below 1.0: MINOR for new things or a new
protocol (receivers of another protocol don't hear it: the broadcast
group's digit changes, `FENNECTV4`), PATCH for fixes. Every release gets
its own number; a number once published is never reused.
