// Stand-in for the platform module "cloudflare:workers", so src/worker.js and src/do.js can be imported by node (test/helpers.mjs registers
// the loader below). The real module exists only inside workerd.
export class DurableObject {
  constructor(ctx, env) { this.ctx = ctx; this.env = env; }
}
