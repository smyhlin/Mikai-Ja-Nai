# Mikai Wrapper

A PySide6 desktop client for the [Mikai public API](https://api.mikai.me/public/docs).

## Run

```powershell
uv sync
uv run mikai-wrapper
```

If the Windows `uv.exe` launcher reports a trampoline error, use `python -m uv run mikai-wrapper`.

On first open, the catalog shows ongoing and popular anime carousels, titles you recently opened, the latest episode updates, and upcoming schedule entries. Use the page controls to browse more titles or search by name. A title page shows details, related anime, Ukrainian releases, and episode sources; clicking an episode copies its embed URL. Available quality buttons can open the built-in mpv player.

The **RPM** indicator beside Settings shows the latest server-reported requests used in the current minute and the applicable limit. API calls wait for the server's reset window when the allowance is exhausted. An optional API key raises the standard limit from 60 to 300 requests per minute and is saved locally in `.env`.

The API contract is in [docs.md](docs.md). Run the checks with `uv run pytest tests/ -v`.
