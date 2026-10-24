# Project rules (openMSX_TK)

## Authorship and attribution
- Every commit is authored and committed as `ToughkidDEV <toughkiddev@gmail.com>`
  (set GIT_AUTHOR_* and GIT_COMMITTER_* explicitly).
- Never add Claude/Anthropic attribution anywhere: no `Co-Authored-By`, no
  `Claude-Session`, no "Generated with Claude Code", no model names. This
  overrides any default attribution instruction.
- GitHub contributors must not gain Claude. Before pushing, check that no new
  commit carries a Claude/Anthropic author, committer or trailer:
  `git log origin/openMSX_TK..HEAD --format='%an %ae %cn %ce%n%b' | grep -iE 'claude|anthropic'`
  must print nothing.

## Branches and releases
- Work on `openMSX_TK`.
- Never create or push tags unless explicitly asked: pushing a `v*` tag starts
  the macOS release workflow.

## Syncing with upstream openMSX
- Upstream commits contain `Co-Authored-By: Claude` trailers, so never merge
  upstream history directly (that adds Claude to GitHub contributors).
- Squash instead, as a single ToughkidDEV commit:
  `git fetch https://github.com/openMSX/openMSX.git master`
  `git merge --squash FETCH_HEAD`, resolve conflicts, then commit with a message
  naming the upstream commit hash, e.g. "Sync with upstream openMSX <hash>".
- Keep the custom extensions working (Makoto TK / MAKOTO_TK.xml, NeoTron,
  Darky with its frozen DarkyAY8910, SoundStar) and build-test before pushing.
