FROM gcc:14-bookworm AS builder
WORKDIR /src
COPY . .
RUN make -j"$(nproc)" all

FROM debian:bookworm-slim
RUN apt-get update && apt-get install -y --no-install-recommends curl ca-certificates && rm -rf /var/lib/apt/lists/* \
    && useradd --system --create-home orbitops
WORKDIR /app
COPY --from=builder /src/build/orbitops /app/orbitops
COPY --from=builder /src/web /app/web
RUN mkdir -p /app/data && chown -R orbitops:orbitops /app
USER orbitops
EXPOSE 8080
ENV ORBITOPS_HOST=0.0.0.0 ORBITOPS_PORT=8080 ORBITOPS_DB=/app/data/orbitops.db ORBITOPS_WEB=/app/web
ENTRYPOINT ["/app/orbitops"]
CMD ["--role", "all"]
