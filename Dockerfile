FROM debian:latest AS builder
RUN apt-get update && apt-get install -y gcc make cmake && rm -rf /var/lib/apt/lists/*
COPY . /ganon
WORKDIR /ganon
RUN make x64d
RUN VERSION=$(cat VERSION) && mv bin/ganon_${VERSION}_x64_debug bin/ganon
FROM gcr.io/distroless/static
COPY --from=builder /ganon/bin/ganon /bin/ganon
ENTRYPOINT ["/bin/ganon"]