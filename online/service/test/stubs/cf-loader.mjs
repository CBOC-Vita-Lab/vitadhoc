// Node module-customisation hook: resolve the platform specifier "cloudflare:workers" to the stub next to this file.
export async function resolve(specifier, context, next) {
  if (specifier === 'cloudflare:workers') return { url: new URL('./cloudflare-workers.mjs', import.meta.url).href, shortCircuit: true };
  return next(specifier, context);
}
