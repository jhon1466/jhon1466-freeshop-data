# functions/

Firebase Cloud Function wrapping the FreeShop catalog API for production
hosting. `src/app.ts`, `src/lib/*`, `src/routes/*`, `src/types/*` and
`shared/catalog.schema.json` are copies of the equivalent files under
`server/` (the standalone Express app used for local dev) - Firebase only
deploys the contents of this directory, so cross-directory imports outside
`functions/` wouldn't be included in the deployed bundle. Keep the two in
sync by hand when changing catalog/admin logic; they're small (a handful of
files).

`src/app.ts` here omits the `/admin`, `/icons` and `/downloads` static routes
present in `server/src/app.ts`: in production those are served directly by
Firebase Hosting (see `../firebase.json`), so this Function only ever needs
to handle `/api/**`.

## Deploy

```
npm run deploy
```

Runs `tsc`, regenerates `../hosting-public` from `admin/` and
`server/public/{icons,downloads}` (see `../scripts/sync-hosting-public.js`),
then `firebase deploy --only functions,hosting`.

Requires (one-time):
- The Firebase project (`yomu-9c07c`, see `../.firebaserc`) on the **Blaze**
  plan - Cloud Functions requires it even for light usage.
- `ADMIN_PASSWORD` and `GITHUB_TOKEN` set as Firebase secrets (not plain env
  vars, since they're sensitive):
  ```
  firebase functions:secrets:set ADMIN_PASSWORD
  firebase functions:secrets:set GITHUB_TOKEN
  ```
  `src/index.ts` binds both via the `secrets` option, so they're available
  as `process.env.*` at runtime same as in `server/`'s `.env`.
