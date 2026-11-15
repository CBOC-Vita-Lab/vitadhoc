// The Durable Object: ONE instance ("main") owns all state, so every request is serialized and the SQLite storage stays consistent
// without locking (Free plan: new_sqlite_classes). It keeps no room in memory between requests, sets no alarm and no timer: Durable Object
// duration is shared with the other games on the account, and an idle object may leave memory after about 10 s (every room is a row).

import { DurableObject } from 'cloudflare:workers';
import { createApp } from './core/app.js';

export class RoomServer extends DurableObject {
  constructor(ctx, env) {
    super(ctx, env);
    this.app = createApp({
      sql: ctx.storage.sql,
      tx: (fn) => ctx.storage.transactionSync(fn),
      env,
    });
  }

  async fetch(request) {
    return this.app.handle(request);
  }
}
