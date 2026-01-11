# UltraBalancer Examples

## Basic Load Balancing

```bash
ultrabalancer config.toml
```

## Docker Setup

```dockerfile
FROM ubuntu:20.04
RUN apt-get update && apt-get install -y ultrabalancer
COPY config.toml /etc/ultrabalancer/
CMD ["ultrabalancer", "/etc/ultrabalancer/config.toml"]
```

## Kubernetes Deployment

Deploy UltraBalancer as a sidecar or service in Kubernetes clusters.
