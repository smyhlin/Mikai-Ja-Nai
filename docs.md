# Публічний API Mikai

> Каталог аніме з українськими озвученнями та субтитрами: назви й зовнішні
> ідентифікатори, плеєр із розбивкою по командах і провайдерах, україномовні
> постери з авторством, розклад виходу серій і стрічка новинок.

- Базова адреса: `https://api.mikai.me/public/v1`
- Документація (HTML): https://api.mikai.me/public/docs
- Специфікація OpenAPI 3.1: `https://api.mikai.me/public/openapi.json`
- Отримати ключ: https://mikai.me/user/developers

## Огляд

Публічний API Mikai віддає каталог аніме з українськими озвученнями та субтитрами: назви, зовнішні ідентифікатори, плеєр із розбивкою по командах і провайдерах, україномовні постери з авторством, розклад виходу серій, стрічку новинок і персонажів.

Усі відповіді — JSON у спільному конверті. Успішна відповідь має `ok: true` і поле `result`; пагіновані списки додатково повертають `total`, `page` і `pages`, тож рахувати кількість сторінок вручну не треба.

```json
{
  "ok": true,
  "total": 1423,
  "page": 1,
  "pages": 72,
  "result": [ … ]
}
```

Усі мітки часу — RFC3339 в UTC: `addedAt`, `publishedAt`, `updatedAt`, час виходу серії в розкладі. Дати без часу (`startDate`, `birthday`) — `YYYY-MM-DD`.

Описи аніме — українською. Це наш власний текст, а не синопсис з MAL чи AniList.

> У блоці `episodes` поле `total` — це заплановане число серій; `0` означає «ще невідомо», а не «серій немає». Скільки вже вийшло, каже `aired`, скільки озвучено або перекладено — `localized`.

> API читальний: жоден ендпоінт не змінює дані на нашому боці. Ключ не обовʼязковий — він лише підіймає ліміти.

## Швидкий старт

Ключ не потрібен, щоб почати. Просто зробіть запит:

```bash
curl "https://api.mikai.me/public/v1/anime?limit=5"
```

Без ключа діють невеликі ліміти — їх вистачає, щоб роздивитись API. Для постійного застосунку створіть ключ у профілі, розділ «Для розробників»: [https://mikai.me/user/developers](https://mikai.me/user/developers). Повний токен показується один раз, одразу після створення.

```bash
curl -H "X-API-Key: mk_xxxxxxxx…" \
  "https://api.mikai.me/public/v1/anime?limit=5"
```

Які ліміти діють саме для вас, показує `/me`.

```bash
curl "https://api.mikai.me/public/v1/me"
```

## Ключ доступу

Ключ необовʼязковий: усі ендпоінти працюють і без нього, просто з меншими лімітами. Ключ підіймає ліміти й дає власний лічильник використання.

Передати його можна двома способами — оберіть той, що зручніший вашому HTTP-клієнту:

```http
X-API-Key: mk_xxxxxxxx…
# або
Authorization: Bearer mk_xxxxxxxx…
```

> Ключ — це секрет. Не вставляйте його у фронтенд-код і в публічні репозиторії: будь-хто, хто його побачить, витрачатиме ваш ліміт.

| Код | Коли трапляється |
| --- | --- |
| 401 | Передано недійсний ключ |
| 403 | Ключ вимкнено |
| 404 | Ресурс не знайдено |
| 422 | Некоректні параметри запиту або посилання на аніме |
| 429 | Перевищено ліміт запитів за хвилину |

## Помилки

Помилка приходить у тому ж конверті з `ok: false`. Поле `code` — стабільний ідентифікатор, на який можна перемикатись у коді; `message` — людський текст українською, який ми можемо переформулювати будь-коли.

```json
{
  "ok": false,
  "error": {
    "errorCode": 422,
    "code": "invalid_parameter",
    "message": "invalid parameter: sort=\"popularity\" is not one of mal_rating, name, updated, views, year"
  }
}
```

| code | HTTP | Що сталося |
| --- | --- | --- |
| invalid_parameter | 422 | Невідоме значення параметра — у повідомленні перелічені допустимі |
| invalid_reference | 422 | Некоректне посилання на аніме у `{ref}` |
| too_many_ids | 422 | У `/resolve` передано понад 100 значень |
| invalid_request | 400 | Тіло запиту не є коректним JSON |
| not_found | 404 | Ресурс не знайдено |
| invalid_api_key | 401 | Передано недійсний ключ |
| api_key_disabled | 403 | Ключ вимкнено |
| rate_limited | 429 | Вичерпано ліміт за хвилину |
| internal_error | 500 | Помилка на нашому боці |

> Невідоме значення фільтра — це `422`, а не порожній список. Так помилка в клієнті не виглядає як «нічого не знайдено».

## Ліміти

Без ключа запити рахуються по IP, з ключем — по ключу. Обмеження є лише на кількість запитів за хвилину, добової квоти немає.

| Спосіб виклику | За хвилину |
| --- | --- |
| Без ключа | 60 |
| З ключем | 300 |

Кожна відповідь містить поточний стан лічильника:

| Заголовок | Значення |
| --- | --- |
| X-RateLimit-Limit | Ліміт запитів за хвилину |
| X-RateLimit-Remaining | Скільки лишилось у поточній хвилині |
| X-RateLimit-Reset | Через скільки секунд лічильник обнулиться |

Після перевищення приходить `429` із заголовком `Retry-After`. Замість того щоб опитувати каталог по колу, використовуйте `/updates?since=…` — вона віддає лише те, що зʼявилося після вказаного моменту.

Потрібно більше — напишіть нам, ліміт конкретного ключа можна підняти.

## Кешування

Кожна успішна відповідь має `ETag` і `Cache-Control`. Надішліть `If-None-Match` з отриманим раніше тегом — і якщо нічого не змінилось, прийде `304` без тіла.

```bash
curl -H 'If-None-Match: "3f2a91c4e0d7"'   "https://api.mikai.me/public/v1/genres"
```

| Що | Живе в кеші |
| --- | --- |
| /genres, /tags, /studios, /meta | 1 година |
| каталог, картка аніме, плеєр, команди | 5 хвилин |
| /schedule, /updates, /posters/ua | 1 хвилина |
| /me, /health, /anime/random | не кешуються |

> `304` так само коштує один запит за лімітом — економія тут у трафіку й розборі JSON, а не в ліміті.

## Ідентифікатори

Скрізь, де у шляху стоїть `{ref}`, можна підставити будь-який відомий ідентифікатор. Без префікса це id Mikai:

```http
GET /anime/1523                    # id Mikai
GET /anime/mal:21                  # MyAnimeList
GET /anime/al:21                   # AniList
GET /anime/hikka:gasdas-one-piece  # Hikka slug
GET /anime/slug:one-piece          # наш slug
```

Той самий `{ref}` працює і для вкладених ресурсів: `/anime/mal:21/player`, `/anime/mal:21/posters`.

Кожна відповідь про аніме містить повний блок `ids`, тож одного запиту досить, щоб дізнатись усі відповідники. IMDb у ньому теж є, але шукати за ним не можна: один IMDb-запис описує всю франшизу, а не окремий сезон.

> Рядок без префікса, який не є числом (наприклад `one-piece`), повертає 422 з підказкою — щоб помилка в клієнті не виглядала як «аніме немає».

Коли треба зіставити багато ідентифікаторів одразу, беріть `/resolve`: до 100 значень за один запит, і за лімітами це коштує один запит.

> Аніме, вкладене в іншу відповідь — у розкладі, стрічці новинок, звʼязаних тайтлах — приходить скорочено: `ids`, `titles` і `images`. Решта полів там просто не читається з бази, тому ми їх і не показуємо; повна картка — за `/anime/{ref}`.

## Зображення

API віддає готові абсолютні посилання — збирати їх вручну не треба. Кожне зображення доступне у трьох розмірах і двох форматах:

```json
{
  "big":    { "webp": "https://images.mikai.me/poster/big/70733847-…webp",
              "jpg":  "https://images.mikai.me/poster/big/70733847-….jpg" },
  "medium": { … },
  "small":  { … }
}
```

| Розмір | Найбільша сторона |
| --- | --- |
| big | 1600 px |
| medium | 900 px |
| small | 400 px |

Файли віддаються з `Cache-Control: immutable` — їх можна безпечно кешувати надовго.

## Використання та атрибуція

Дані можна використовувати у власних застосунках, ботах і сайтах. Просимо про три речі:

- вказуйте джерело — посилання на [mikai.me](https://mikai.me);
- зберігайте авторство команд озвучення та авторів постерів — воно приходить у полях `team` та `author`;
- не використовуйте API для масового дзеркалювання відео — посилання на плеєри належать провайдерам.

Номер серії — дробове число. Команда, яка озвучує здвоєні (половинчасті) серії окремо, має `1` і `1.5` — це одна канонічна серія. Тому `episodesCount` рахує канонічні серії й може бути меншим за довжину масиву `episodes`, а `lastEpisode` — це найбільший номер, якого команда досягла.

Аніме під жорсткою українською ліцензією повертаються без джерел: `licensed: true` і порожній `releases`. Блок `license` при цьому каже, хто ліцензіат і де дивитись легально — покажіть це замість порожнього плеєра. Інші ліцензії на видачу не впливають.

## Версії та сумісність

Версія контракту приходить у `/meta` як `version`. У межах `/public/v1` ми можемо **додавати** нові поля й ендпоінти — тому ваш клієнт має ігнорувати незнайомі поля, а не падати на них.

- нове поле у відповіді або новий необовʼязковий параметр — не ламає сумісність і може зʼявитись будь-коли;
- видалення чи перейменування поля, зміна типу — тільки в новій версії шляху (`/public/v2`);
- стару версію ми тримаємо щонайменше пів року після виходу нової й попереджаємо в [контактах](https://mikai.me/contacts).

> Значення enum-полів теж можуть поповнюватись. Замість того щоб зашивати списки, звіряйтесь із `/meta` — там і формати, і статуси, і допустимі поля сортування.

## Аніме

Каталог, детальна інформація та схожі тайтли.

### Список аніме

`GET /public/v1/anime`

Пагінований каталог з короткою інформацією. Фільтри збігаються з тими, що працюють на сайті. Якщо передано `search`, результати впорядковані за релевантністю, а `sort` і `order` ігноруються.

| Параметр | Де | Тип | Опис |
| --- | --- | --- | --- |
| `search` | query | `string` | Пошук за назвою (українською, англійською, оригінальною або будь-якою з альтернативних) |
| `genres` | query | `string[]` | Жанри через кому — значення поля `name` з `/genres` |
| `genresExclude` | query | `string[]` | Жанри, яких не має бути |
| `genresMatch` | query | `string` = `all` | `all` — усі перелічені жанри, `any` — будь-який із них |
| `tags` | query | `string[]` | Теги через кому — значення поля `name` з `/tags` |
| `tagsExclude` | query | `string[]` | Теги, яких не має бути |
| `tagsMatch` | query | `string` = `all` | `all` або `any` |
| `years` | query | `string[]` | Конкретні роки через кому, наприклад `1999,2003` |
| `yearFrom` | query | `int` | Рік від |
| `yearTo` | query | `int` | Рік до |
| `seasons` | query | `string[]` | winter, spring, summer, autumn, unknown |
| `formats` | query | `string[]` | tv, movie, special, ova, ona, music, other, unknown |
| `statuses` | query | `string[]` | finished, ongoing, announce, cancelled, break, unknown |
| `studios` | query | `string[]` | Студії через кому |
| `teams` | query | `int[]` | ID команд озвучення через кому |
| `sort` | query | `string` = `name` | name, year, updated (за останньою доданою серією), views, mal_rating |
| `order` | query | `string` | asc або desc. За замовчуванням `asc` для `name` і `desc` для решти полів |
| `page` | query | `int` = `1` | Номер сторінки |
| `limit` | query | `int` = `20` | Розмір сторінки, максимум 100 |

Запит:

```bash
curl "https://api.mikai.me/public/v1/anime?statuses=ongoing&limit=2"
```

Відповідь:

```json
{
  "ok": true,
  "total": 214,
  "page": 1,
  "pages": 107,
  "result": [
    {
      "ids": { "mikai": 1523, "slug": "one-piece", "mal": 21,
               "al": 21, "hikka": "gasdas-one-piece", "imdb": "tt0388629" },
      "titles": { "ua": "Ван Піс", "english": "One Piece", "original": "ワンピース" },
      "images": {
        "poster": { "big": { "webp": "https://images.mikai.me/poster/big/….webp",
                             "jpg":  "https://images.mikai.me/poster/big/….jpg" } }
      },
      "format": "tv",
      "status": "ongoing",
      "season": "autumn",
      "year": 1999,
      "episodes": 0,
      "isAdult": false
    }
  ]
}
```

### Детальна інформація

`GET /public/v1/anime/{ref}`

Усе про тайтл одним запитом: опис, жанри й теги, оцінки, вікове обмеження, країна, класифікація серій (філери/канон), звʼязані тайтли, схожі, україномовні постери та зведення релізів. Окремо доводиться питати лише список серій — він на `/anime/{ref}/player`.

| Параметр | Де | Тип | Опис |
| --- | --- | --- | --- |
| `ref` (обовʼязковий) | path | `string` | Будь-який ідентифікатор: 1523, mal:21, al:21, hikka:…, slug:… |
| `include` | query | `string[]` | Звузити відповідь до потрібних блоків: releases, uaPosters, relations, similar. За замовчуванням приходять усі |

Запит:

```bash
curl "https://api.mikai.me/public/v1/anime/mal:21"
```

Відповідь:

```json
{
  "ok": true,
  "result": {
    "ids": { "mikai": 1523, "slug": "one-piece", "mal": 21, "al": 21 },
    "titles": { "ua": "Ван Піс", "english": "One Piece", "synonyms": ["ワンピース", "One Piece: WE ARE!"] },
    "description": "…",
    "format": "tv",
    "status": "ongoing",
    "startDate": "1999-10-20",
    "source": "manga",
    "studio": "Toei Animation",
    "country": "JP",
    "ageRating": "pg13",
    "episodes": { "total": 0, "aired": 1122, "localized": 640, "durationMin": 24 },
    "scores": { "mikai": 9.1, "mikaiCount": 812, "mal": 8.73, "malCount": 1400000 },
    "genres": [ { "name": "Action", "ua": "Бойовик" } ],
    "tags": [ { "name": "Pirates", "ua": "Пірати" } ],
    "episodeTypes": { "filler": [ { "from": 54, "to": 60 } ] },
    "licensed": false,
    "releases": [
      {
        "id": "t_45:voice",
        "kind": "voice",
        "isCollab": false,
        "teams": [ { "id": 45, "slug": "fanvoxua", "name": "FanVoxUA" } ],
        "episodesCount": 640,
        "lastEpisode": 640,
        "updatedAt": "2026-08-20T10:00:00Z"
      }
    ],
    "uaPosters": [ { "id": 7, "isSelected": true, "author": "Оксана К.", "team": { … }, "images": { … } } ],
    "relations": [ { "kind": "sideStory", "anime": { … } } ],
    "similar": [ { "ids": { "mikai": 902, "mal": 6702 }, "titles": { "ua": "Фейрі Тейл" }, "format": "tv", … } ]
  }
}
```

### Випадкове аніме

`GET /public/v1/anime/random`

Коротка картка випадкового тайтлу.

Запит:

```bash
curl "https://api.mikai.me/public/v1/anime/random"
```

### Резолв ідентифікаторів (GET)

`GET /public/v1/resolve`

Те саме, що POST-версія, але списком у query — зручно перевірити руками або з клієнта, який не вміє слати тіло в GET.

| Параметр | Де | Тип | Опис |
| --- | --- | --- | --- |
| `source` (обовʼязковий) | query | `string` | mal, al, hikka, slug або mikai |
| `ids` (обовʼязковий) | query | `string[]` | До 100 значень через кому |

Запит:

```bash
curl "https://api.mikai.me/public/v1/resolve?source=mal&ids=1,21,999999999"
```

### Батч-резолв ідентифікаторів

`POST /public/v1/resolve`

Зіставляє до 100 зовнішніх ідентифікаторів з нашими за один запит. Значення, яких у нас немає, повертаються як null.

| Параметр | Де | Тип | Опис |
| --- | --- | --- | --- |
| `source` (обовʼязковий) | body | `string` | mal, al, hikka, slug або mikai |
| `ids` (обовʼязковий) | body | `array` | До 100 значень |

Запит:

```bash
curl -X POST -H "Content-Type: application/json" \
  -d '{"source":"mal","ids":[1,21,999999999]}' \
  "https://api.mikai.me/public/v1/resolve"
```

Відповідь:

```json
{
  "ok": true,
  "result": {
    "1":  { "mikai": 12, "slug": "cowboy-bebop", "mal": 1, "al": 1 },
    "21": { "mikai": 1523, "slug": "one-piece", "mal": 21, "al": 21 },
    "999999999": null
  }
}
```

## Плеєр

Озвучення та субтитри, згруповані по релізах.

### Серії та джерела

`GET /public/v1/anime/{ref}/player`

Список серій. Реліз — це одна команда (або колаборація) і один трек: озвучення `voice` чи субтитри `sub`. Усередині релізу серії лежать плоским списком, а кожна серія містить масив `sources` з усіма провайдерами, які її хостять. Якщо серії не потрібні, те саме зведення релізів уже є в детальній картці аніме.

| Параметр | Де | Тип | Опис |
| --- | --- | --- | --- |
| `ref` (обовʼязковий) | path | `string` | Ідентифікатор аніме |
| `episodes` | query | `bool` = `true` | `false` — повернути тільки зведення релізів, без списку серій |

Запит:

```bash
curl "https://api.mikai.me/public/v1/anime/mal:21/player"
```

Відповідь:

```json
{
  "ok": true,
  "result": {
    "anime": { "ids": { "mikai": 1523, "mal": 21 }, "titles": { "ua": "Ван Піс" } },
    "licensed": false,
    "releases": [
      {
        "id": "t_45:voice",
        "kind": "voice",
        "isCollab": false,
        "teams": [ { "id": 45, "slug": "fanvoxua", "name": "FanVoxUA" } ],
        "episodesCount": 640,
        "lastEpisode": 640,
        "updatedAt": "2026-08-20T10:00:00Z",
        "episodes": [
          {
            "number": 640,
            "label": "640",
            "kind": "mangaCanon",
            "addedAt": "2026-08-20T10:00:00Z",
            "sources": [
              { "provider": "ashdi", "embedUrl": "https://…", "addedAt": "2026-08-20T10:00:00Z" },
              { "provider": "moon",  "embedUrl": "https://…", "addedAt": "2026-08-20T10:02:00Z" }
            ]
          }
        ]
      }
    ]
  }
}
```

### Стрічка новинок

`GET /public/v1/updates`

Щойно опубліковані серії по всьому сайту, найновіші першими. Це найдешевший спосіб тримати дзеркало або канал у Telegram актуальним: передавайте `since` з часом попереднього запиту. Поле `teams` влаштоване так само, як у плеєрі: для колаборації воно містить усі команди, що працювали над релізом, і `isCollab: true`. Одна серія, яку хостять кілька провайдерів, приходить окремим записом на кожного.

| Параметр | Де | Тип | Опис |
| --- | --- | --- | --- |
| `since` | query | `string` | Мітка часу RFC3339 — повернути лише те, що зʼявилось пізніше |
| `page` | query | `int` = `1` | Номер сторінки |
| `limit` | query | `int` = `20` | Розмір сторінки, максимум 100 |

Запит:

```bash
curl "https://api.mikai.me/public/v1/updates?since=2026-08-25T00:00:00Z&limit=50"
```

Відповідь:

```json
{
  "ok": true,
  "total": 87,
  "result": [
    {
      "anime": { "ids": { "mikai": 1523, "mal": 21 }, "titles": { "ua": "Ван Піс" } },
      "teams": [ { "id": 45, "slug": "fanvoxua", "name": "FanVoxUA" } ],
      "isCollab": false,
      "teamName": "FanVoxUA",
      "kind": "voice",
      "provider": "ashdi",
      "episode": 640,
      "embedUrl": "https://…",
      "publishedAt": "2026-08-25T18:12:00Z"
    }
  ]
}
```

## Постери

Україномовні постери з інформацією про авторство.

### Постери одного аніме

`GET /public/v1/anime/{ref}/posters`

Оригінальна обкладинка плюс усі україномовні постери. Для кожного приходить команда, яка його опублікувала, та особистий автор, якщо він відомий.

| Параметр | Де | Тип | Опис |
| --- | --- | --- | --- |
| `ref` (обовʼязковий) | path | `string` | Ідентифікатор аніме |

Запит:

```bash
curl "https://api.mikai.me/public/v1/anime/mal:21/posters"
```

Відповідь:

```json
{
  "ok": true,
  "result": {
    "anime": { "ids": { "mikai": 1523, "mal": 21 } },
    "original": { "big": { "webp": "https://images.mikai.me/poster/big/….webp", "jpg": "…" } },
    "uaPosters": [
      {
        "id": 7,
        "isSelected": true,
        "author": "Оксана К.",
        "team": { "id": 45, "slug": "fanvoxua", "name": "FanVoxUA" },
        "images": {
          "big":    { "webp": "https://images.mikai.me/ua_poster/big/70733847-7883-11f1-b061-96742946622a.webp",
                      "jpg":  "https://images.mikai.me/ua_poster/big/70733847-7883-11f1-b061-96742946622a.jpg" },
          "medium": { … },
          "small":  { … }
        }
      }
    ]
  }
}
```

### Стрічка всіх постерів

`GET /public/v1/posters/ua`

Усі україномовні постери з пагінацією — для дзеркал і галерей. Кожен запис несе аніме, до якого належить.

| Параметр | Де | Тип | Опис |
| --- | --- | --- | --- |
| `since` | query | `string` | Мітка часу RFC3339 |
| `page` | query | `int` = `1` | Номер сторінки |
| `limit` | query | `int` = `20` | Розмір сторінки, максимум 100 |

Запит:

```bash
curl "https://api.mikai.me/public/v1/posters/ua?limit=50"
```

## Команди

Команди озвучення та субтитрування і їхні роботи.

### Список команд

`GET /public/v1/teams`

| Параметр | Де | Тип | Опис |
| --- | --- | --- | --- |
| `search` | query | `string` | Пошук за назвою |
| `haveVoice` | query | `bool` | Лише ті, хто озвучує |
| `haveSubs` | query | `bool` | Лише ті, хто робить субтитри |
| `sort` | query | `string` = `name` | name, count (кількість тайтлів), updated (за останнім релізом) |
| `order` | query | `string` | asc або desc. За замовчуванням `asc` для `name` і `desc` для решти |
| `page` | query | `int` = `1` | Номер сторінки |
| `limit` | query | `int` = `20` | Розмір сторінки, максимум 50 |

Запит:

```bash
curl "https://api.mikai.me/public/v1/teams?haveVoice=true&limit=5"
```

### Інформація про команду

`GET /public/v1/teams/{ref}`

Приймає числовий id або slug команди. `views` — скільки разів дивилися серії команди на Mikai; серії, випущені в колаборації, зараховуються кожній команді з неї.

| Параметр | Де | Тип | Опис |
| --- | --- | --- | --- |
| `ref` (обовʼязковий) | path | `string` | ID або slug команди |

Запит:

```bash
curl "https://api.mikai.me/public/v1/teams/fanvoxua"
```

Відповідь:

```json
{
  "ok": true,
  "result": {
    "id": 45,
    "slug": "fanvoxua",
    "name": "FanVoxUA",
    "description": "…",
    "birthday": "2018-04-12",
    "avatar": { "big": { "webp": "…", "jpg": "…" } },
    "links": { "telegram": "https://t.me/…", "youtube": "https://…" },
    "hasVoice": true,
    "hasSubs": false,
    "animeCount": 128,
    "views": { "total": 4210533, "last30Days": 118904 }
  }
}
```

### Роботи команди

`GET /public/v1/teams/{ref}/anime`

| Параметр | Де | Тип | Опис |
| --- | --- | --- | --- |
| `ref` (обовʼязковий) | path | `string` | ID або slug команди |
| `page` | query | `int` = `1` | Номер сторінки |
| `limit` | query | `int` = `20` | Розмір сторінки, максимум 100 |

Запит:

```bash
curl "https://api.mikai.me/public/v1/teams/fanvoxua/anime"
```

## Персонажі

Пошук персонажів за будь-яким із їхніх імен та детальна картка.

### Пошук персонажів

`GET /public/v1/characters`

Шукає за українським, англійським, оригінальним та альтернативними іменами — з поправкою на одруківки, як пошук у каталозі. Якщо збіг слабкий, пробуємо й українське написання запиту (`микаса` → `мікаса`). Без `search` це просто каталог персонажів, упорядкований за популярністю. `mainAnime` — аніме, яким персонаж найвідоміший: спершу головна роль, потім найновіше; `role` у ньому — роль персонажа саме там.

| Параметр | Де | Тип | Опис |
| --- | --- | --- | --- |
| `search` | query | `string` | Імʼя персонажа будь-якою з мов. Потрібно щонайменше дві літери чи цифри; довший за 100 символів обрізається |
| `roles` | query | `string[]` | main, supporting, background через кому — лише ті, хто має таку роль хоча б в одному аніме |
| `genders` | query | `string[]` | male, female, nonbinary, other через кому |
| `ageFrom` | query | `int` | Вік від. Вік персонажа — проміжок (`17–19`), він підходить, якщо перетинається з вашим |
| `ageTo` | query | `int` | Вік до |
| `birthMonth` | query | `int` | Місяць народження, 1–12 |
| `birthToday` | query | `bool` | Лише ті, в кого день народження сьогодні за київським часом. Має перевагу над `birthMonth` |
| `withDescription` | query | `bool` | Лише з описом українською |
| `anime` | query | `int` | ID аніме — лише персонажі з нього |
| `sort` | query | `string` = `relevance` | relevance (потребує `search`), popularity, name, anime (кількість аніме), birthday (з січня; без дня народження — в кінці) |
| `order` | query | `string` | asc або desc. За замовчуванням `asc` для `name` і `desc` для решти |
| `page` | query | `int` = `1` | Номер сторінки |
| `limit` | query | `int` = `20` | Розмір сторінки, максимум 50 |

Запит:

```bash
curl "https://api.mikai.me/public/v1/characters?search=луффі&limit=2"
```

Відповідь:

```json
{
  "ok": true,
  "total": 4,
  "page": 1,
  "limit": 2,
  "result": [
    {
      "id": 757,
      "slug": "luffy-monkey-d",
      "names": { "ua": "Монкі Д. Луффі", "en": "Luffy Monkey D.", "native": "モンキー・D・ルフィ" },
      "image": { "webp": "…/character/small/….webp", "jpg": "…/character/small/….jpg" },
      "animeCount": 16,
      "favorites": 185845,
      "mainAnime": { "id": 21, "slug": "van-pis", "name": "Ван Піс", "nameEn": "One Piece", "year": 1999, "format": "tv", "role": "main" }
    },
    {
      "id": 51234,
      "slug": "lufas-mafaal",
      "names": { "ua": "Луфас Мафаагл", "en": "Lufas Maphaahl" },
      "animeCount": 1,
      "favorites": 412,
      "mainAnime": { "id": 7688, "slug": "…", "name": "Зʼявився дикий останній бос!", "year": 2025, "format": "tv", "role": "main" }
    }
  ]
}
```

### Інформація про персонажа

`GET /public/v1/characters/{ref}`

Приймає числовий id, slug або рядок `id-slug` з адреси сторінки. Обʼєднаного персонажа віддаємо як того, у кого його обʼєднали. Імена зі спойлерами в `altNames` не потрапляють.`description` — Markdown українською, а якщо українського опису ще немає — англійською. Посилання в ньому ведуть на сторінки Mikai. Спойлери позначені директивами: блоковий — рядками `::spoiler` … `::`, у реченні — `:inline-spoiler[текст]`; якщо ваш рендерер їх не знає, сховайте або приберіть цей текст. `birthday` — ISO-дата, а без відомого року — `--MM-DD`.

| Параметр | Де | Тип | Опис |
| --- | --- | --- | --- |
| `ref` (обовʼязковий) | path | `string` | ID, slug або `id-slug` |

Запит:

```bash
curl "https://api.mikai.me/public/v1/characters/757-luffy-monkey-d"
```

Відповідь:

```json
{
  "ok": true,
  "result": {
    "id": 757,
    "slug": "luffy-monkey-d",
    "names": { "ua": "Монкі Д. Луффі", "en": "Luffy Monkey D.", "native": "モンキー・D・ルフィ" },
    "image": { "webp": "…", "jpg": "…" },
    "animeCount": 16,
    "favorites": 185845,
    "mainAnime": { "id": 21, "slug": "van-pis", "name": "Ван Піс", "nameEn": "One Piece", "year": 1999, "format": "tv", "role": "main" },
    "altNames": ["Бриль", "Муґівара", "Straw Hat Luffy"],
    "description": "Широко відомий як «Бриль Луффі»… Він шукає скарб покійного [Ґол Д. Роджера](https://mikai.me/characters/…).",
    "gender": "male",
    "ageMin": 17,
    "ageMax": 17,
    "birthday": "--05-05",
    "heightCm": 172,
    "externalIds": { "hikka": ["luffy-monkey-d-b6c098"], "mal": ["40"], "anilist": ["40"] },
    "anime": [
      { "id": 21, "slug": "van-pis", "name": "Ван Піс", "nameEn": "One Piece", "year": 1999, "format": "tv", "role": "main" }
    ],
    "updatedAt": "2026-09-18T01:31:55Z"
  }
}
```

## Транслітерація

Українська транслітерація імен і назв: японська за Коваленком, китайська за Кірносовою–Цісар, корейська за Щегелем, західні — за чинним правописом.

### Транслітерувати текст

`GET /public/v1/translit`

Той самий транслітератор, яким ми пишемо імена персонажів і акторів на сайті. Кожен рядок обробляється окремо, розділові знаки й порожні рядки зберігаються. Слово, яке система не може прочитати (ієрогліфи, латинка в корейському режимі), повертається без змін і потрапляє у `unresolved`. Порядок слів не міняється. Якщо жодне слово не прочиталось, а інша мова читає текст повністю, у відповіді зʼявляється `suggest` — мова, назва розділу й готове читання там. Для `lang=en` режим імені розрізняє людей і все інше: `mode=text` читає будь-що, крім імені людини, як власну назву: и після дж, ж, ч, ш, щ, ц і р (Chicago → Шикаґо, Toshiba → Тошиба), `auto` і `name` — як імена (і після приголосного). Імʼя, яке в нашому каталозі щонайменше троє записів пишуть однаково, віддається в цій формі, приведеній до правил (ґ, і): такий рядок має `from: catalog` і `rules` із читанням за правилами. Машинні переклади й поодинокі записи в цьому не беруть участі.

| Параметр | Де | Тип | Опис |
| --- | --- | --- | --- |
| `lang` (обовʼязковий) | query | `string` | `ja` — ромаджі або кана, `zh` — піньїнь, `ko` — ханґиль, `en` — латинка |
| `text` (обовʼязковий) | query | `string` | Текст, до 32 КБ. Синонім — `q` |
| `system` | query | `string` | Друга система мови, якщо вона є. `lang=ja`: `kovalenko` (типово) або `gojuon` — система Ґоджюон (КНУ, 2011), яка не йотує голосний на стику складів (Кейічі замість Кейїчі). `lang=zh`: `ukt` (типово) або `academic` — академічна система НАН 2019 (цзі, чжа, -ун). `lang=ko`: `hikka` (типово) — читає слово як воно звучить, або `shchegel` — академічна система ще зі складами імені через дефіс |
| `mode` | query | `string` = `auto` | Що саме в тексті. `name` — кожен рядок є одним особовим імʼям (прізвище, далі імʼя), `text` — звичайний текст, ніколи не читати слово як імʼя. Діє для `lang=ko` (межа «прізвище + імʼя») , `lang=ja` (кінцеві частини імен -ishi, -ura, -izumi, -uchi… як окремі морфеми), `lang=zh` (складене імʼя зшивається: Mao Ze Dong → Мао Дзедон) та `lang=en` (імена пишуться з і, а `text` читає все інше як власну назву: Chicago → Шикаґо) |

Запит:

```bash
curl "https://api.mikai.me/public/v1/translit?lang=ja&text=Gojou%20Satoru"
```

Відповідь:

```json
{
  "ok": true,
  "result": {
    "lang": "ja",
    "system": "Система Коваленка",
    "text": "Ґоджьо Сатору",
    "lines": [
      { "source": "Gojou Satoru", "result": "Ґоджьо Сатору" }
    ]
  }
}
```

### Транслітерувати список

`POST /public/v1/translit`

Те саме, але тілом запиту. Передайте `text` з переносами рядків або масив `items` — відповідь та сама, по рядку на елемент.

| Параметр | Де | Тип | Опис |
| --- | --- | --- | --- |
| `lang` (обовʼязковий) | body | `string` | ja, zh, ko або en |
| `text` | body | `string` | Багаторядковий текст |
| `items` | body | `string[]` | Список рядків замість `text` |
| `system` | body | `string` | ja: kovalenko (типово) або gojuon; zh: ukt (типово) або academic; ko: hikka (типово) або shchegel |
| `mode` | body | `string` = `auto` | auto, name або text — див. GET |

Запит:

```bash
curl -X POST "https://api.mikai.me/public/v1/translit"   -H "Content-Type: application/json"   -d '{"lang":"zh","items":["Xie Wanqian","Mao Zedong"]}'
```

Відповідь:

```json
{
  "ok": true,
  "result": {
    "lang": "zh",
    "system": "Система Кірносової–Цісар (УКТ)",
    "text": "Сє Ваньцянь
Мао Дзедон",
    "lines": [
      { "source": "Xie Wanqian", "result": "Сє Ваньцянь" },
      { "source": "Mao Zedong",  "result": "Мао Дзедон" }
    ]
  }
}
```

### Правила та джерела

`GET /public/v1/translit/systems`

Опис кожної системи: правила з прикладами, типові русифіковані варіанти, обмеження й посилання на джерела. Якщо в мови є друга, необовʼязкова система, вона приходить у полі `alt` разом із параметром, який її вмикає (китайська: академічна система 2019; корейська: система Hikka). Поле `nameMode` каже, що для системи має сенс `mode=name`. У `naming` — редакційне правило запису імені (порядок слів тощо): його застосовує людина, сама служба порядок ніколи не міняє. Саме цей опис показує [trans.mikai.me](https://trans.mikai.me).

Запит:

```bash
curl "https://api.mikai.me/public/v1/translit/systems"
```

Відповідь:

```json
{
  "ok": true,
  "total": 4,
  "result": [
    {
      "lang": "ja",
      "title": "Японська",
      "system": "Система Коваленка",
      "accepts": [ { "name": "Ромаджі (Гепберн)", "example": "Gojou Satoru" }, { "name": "Хіраґана", "example": "しんいち" }, … ],
      "summary": "Практична транскрипція О. Коваленка (2012)…",
      "rules":   [ { "text": "し ち つ ふ じ → ші чі цу фу джі", "example": "Fuji → Фуджі" } ],
      "against": [ { "bad": "Сусі, Тіба, Дзюн", "good": "Суші, Чіба, Джюн", "why": "«Поліванівка»…" } ],
      "limits":  [ "Ієрогліфи не читаються…" ],
      "sources": [ { "title": "Стаття модераторів Hikka", "url": "https://hikka.io/articles/…" } ]
    }
  ]
}
```

## Довідники

Розклад, словники та самоопис API.

### Розклад виходу серій

`GET /public/v1/schedule`

Обʼєкт із сімома ключами — від `monday` до `sunday`. Усі дні присутні завжди, навіть порожні. День визначається за київським часом виходу серії, а саме поле `airing` приходить у UTC.

Запит:

```bash
curl "https://api.mikai.me/public/v1/schedule"
```

Відповідь:

```json
{
  "ok": true,
  "result": {
    "monday": [
      {
        "anime": { "ids": { "mikai": 1523, "mal": 21 }, "titles": { "ua": "Ван Піс" } },
        "episode": 641,
        "airing": "2026-08-31T06:30:00Z",
        "finished": false
      }
    ],
    "tuesday": []
  }
}
```

### Жанри

`GET /public/v1/genres`

Канонічні назви жанрів та їхні українські відповідники. Значення поля `name` — те, що приймає фільтр `genres`.

Запит:

```bash
curl "https://api.mikai.me/public/v1/genres"
```

### Теги

`GET /public/v1/tags`

Канонічні назви тегів та їхні українські відповідники. Значення поля `name` — те, що приймає фільтр `tags`.

Запит:

```bash
curl "https://api.mikai.me/public/v1/tags"
```

### Студії

`GET /public/v1/studios`

Перелік студій-виробників для фільтра `studios`.

Запит:

```bash
curl "https://api.mikai.me/public/v1/studios"
```

### Самоопис API

`GET /public/v1/meta`

Усі допустимі значення enum-полів і полів сортування, розміри та формати зображень, чинні ліміти й версія контракту. Зручно звіряти клієнт із живим сервером замість того, щоб зашивати списки в код.

Запит:

```bash
curl "https://api.mikai.me/public/v1/meta"
```

Відповідь:

```json
{
  "ok": true,
  "result": {
    "version": "1.0",
    "formats": [ "tv", "movie", … ],
    "statuses": [ "finished", "ongoing", … ],
    "relationKinds": [ "prequel", "sequel", "sideStory", … ],
    "animeSortFields": [ "mal_rating", "name", "updated", "views", "year" ],
    "teamSortFields": [ "count", "name", "updated" ],
    "sortOrders": [ "asc", "desc" ],
    "matchModes": [ "all", "any" ],
    "detailIncludes": [ "releases", "uaPosters", "relations", "similar" ],
    "limits": {
      "anonymous": { "rateLimitPerMin": 60 },
      "withKey":   { "rateLimitPerMin": 300 }
    },
    "maxPageSize": 100,
    "maxTeamPageSize": 50
  }
}
```

### Поточні ліміти

`GET /public/v1/me`

Які ліміти діють для цього виклику і скільки з них уже витрачено. Без ключа повертає ліміти анонімного доступу.

Запит:

```bash
curl "https://api.mikai.me/public/v1/me"
```

### Перевірка доступності

`GET /public/v1/health`

Запит:

```bash
curl "https://api.mikai.me/public/v1/health"
```
