# Mikai API endpoint coverage inspection

Date: 2026-10-12. Question asked: *are there new Mikai API endpoints we still do
not use?* Answer: the whole documented surface is live and unchanged, and this
app deliberately uses 6 of the 22 endpoints. Nothing is missing because it is
new; the unused ones are unused by design.

## The endpoint surface, and what this app calls

The app calls exactly **six** of them — `/anime`, `/anime/{ref}`,
`/anime/{ref}/player`, `/updates`, `/schedule`, `/me` — and nothing else.

| Endpoint | Live | Used by this app | Why |
|---|---|---|---|
| `GET /anime` | 200 | **yes** | search + browse (ongoing, popular) |
| `GET /anime/{ref}` | 200 | **yes** | detail; carries `relations`, `similar`, `genres`, `tags`, `scores`, `releases`, `uaPosters` |
| `GET /anime/{ref}/player` | 200 | **yes** | per-release tabs + episode grids |
| `GET /updates` | 200 | **yes** | "Останні оновлення" feed |
| `GET /schedule` | 200 | **yes** | "Графік виходу" feed |
| `GET /me` | 200 | **yes** | quota panel (keyed only) |
| `GET /meta` | 200 | no | contract self-description; the client is version-agnostic and reads image sizes off each payload, so there is no caller. Worth calling only if a future surface needs to branch on `version` |
| `GET /anime/random` | 200 | no | no-cache by contract; nothing in the product asks for random |
| `GET /resolve` | 200 | no | see below |
| `POST /resolve` | 200 | no | see below |
| `GET /anime/{ref}/posters` | 200 | no | `uaPosters` already arrives inside the detail payload |
| `GET /posters/ua` | 200 | no | 1378×689 pages of cover art; no product surface browses it |
| `GET /teams`, `/teams/{ref}`, `/teams/{ref}/anime` | 200 | no | teams already arrive per release inside `/player` |
| `GET /characters`, `/characters/{ref}` | 200 | no | no character surface in this product |
| `GET /translit`, `POST /translit`, `/translit/systems` | 200 | no | no translit surface |
| `GET /genres`, `/tags`, `/studios` | 200 | no | `genres` and `tags` arrive embedded in the detail payload (parsed at `Models.cpp:280-281`), so the dictionary endpoints are redundant here; `studios` is not parsed |
| `GET /health` | 200 | no | ops probe, not a client concern |

## What was actually probed

* All 22 documented endpoints were requested: **all 200**. `/meta` still reports
  contract `version: "1.0"`, the same `imageOrigin`, and an unchanged enumeration
  of formats, statuses, seasons, sources, providers (`moon`, `ashdi`, `tortuga`,
  `peertube`), release kinds and episode kinds.
* ~25 undocumented paths were probed (`/anime/{ref}/relations`, `/anime/{ref}/
  similar`, `/anime/{ref}/episodes`, `/anime/{ref}/characters`, `/search`,
  `/franchise`, `/reviews`, `/openapi.json`, ...). Every one returns **405**, and
  405 is returned for both GET and POST on paths that do not exist at all - it is
  the router's response to an unknown sub-path, not a method mismatch. There is
  no hidden sub-resource under `/anime/{ref}`.

## Fields the detail payload carries that this app does not parse

The live `/anime/{ref}` response has **25** top-level keys. Unparsed:

| Field | What it is | Assessment |
|---|---|---|
| `nextEpisode` | the next scheduled episode | unused - `/schedule` and the release tab already answer "what airs next" for this product |
| `episodeTypes` | per-episode filler/canon classification | unused - the tab grid shows numbers, not filler flags |
| `isAdult` | adult flag | unused - no such surface |
| `images.banner` | wide banner | unused - `images.poster` and `images.uaPoster` feed the cover and the zoomable viewer |

These are parsed-forward-safe: the contract promises additive fields and the
client already ignores unknown keys, so this is a coverage list, not a defect
list.

## The one pair worth a second thought

`/resolve` and `POST /resolve` map an external id (`mal`, `al`, `hikka`, `imdb`)
to a Mikai slug in one request, with no enumeration or search. The app accepts
`mal:21`-style refs as input - `TitlePage` and `getAnime` both take one - but
the API resolves them server-side, so nothing client-side needs `/resolve`. It
becomes relevant only if a surface is added that lists *external* ids it does not
already hold (an import/import-list flow), which this product does not have.

## Conclusion

No new endpoint is being missed. The gap is a *coverage* choice, and the two
endpoints that would change what a user can do are `characters` (a whole surface
this product has never had) and `translit` (a search-spelling helper). If the
owner wants either, that is a feature, not an upgrade.
