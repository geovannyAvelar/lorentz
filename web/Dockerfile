# Multi-stage build producing a small runtime image (Next.js's standalone
# output, see next.config.ts). Build from the web/ directory:
#   docker build -t lorentz-web web/
# Run pointed at a Lorentz instance:
#   docker run -p 3000:3000 -e LORENTZ_API_URL=http://lorentz:80 lorentz-web

FROM node:20-alpine AS deps
WORKDIR /app
COPY package.json package-lock.json ./
RUN npm ci

FROM node:20-alpine AS build
WORKDIR /app
COPY --from=deps /app/node_modules ./node_modules
COPY . .
# LORENTZ_API_URL is read at request time (route handlers), not at build
# time, so no build-time value is needed here.
RUN npm run build

FROM node:20-alpine AS run
WORKDIR /app
ENV NODE_ENV=production
RUN addgroup --system --gid 1001 nodejs && adduser --system --uid 1001 nextjs
COPY --from=build --chown=nextjs:nodejs /app/.next/standalone ./
COPY --from=build --chown=nextjs:nodejs /app/.next/static ./.next/static
COPY --from=build --chown=nextjs:nodejs /app/public ./public
USER nextjs
EXPOSE 3000
ENV PORT=3000 HOSTNAME=0.0.0.0
CMD ["node", "server.js"]
