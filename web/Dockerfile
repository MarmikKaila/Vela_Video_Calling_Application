# Container image for the signaling server (free hosts: fly.io, Render, etc.).
# Runs plain HTTP behind the platform's edge TLS (browsers still see HTTPS, so
# camera/mic work). STUN/TURN are served to clients via /config.
FROM node:20-alpine
WORKDIR /app
COPY package*.json ./
RUN npm install --omit=dev
COPY . .
ENV PLAIN_HTTP=1
ENV PORT=8080
EXPOSE 8080
CMD ["node", "server.js"]
