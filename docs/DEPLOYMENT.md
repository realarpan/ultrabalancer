# Deployment Guide for UltraBalancer

## Production Deployment

This guide covers deploying UltraBalancer in production environments.

### Prerequisites
- Ubuntu 20.04 LTS or CentOS 8+
- 4GB+ RAM
- 2+ CPU cores
- Docker (optional)

### Installation Steps

1. **Binary Installation**
   ```bash
   wget https://github.com/realarpan/ultrabalancer/releases/download/v2.0.1/ultrabalancer
   chmod +x ultrabalancer
   sudo mv ultrabalancer /usr/local/bin/
   ```

2. **Configuration**
   ```bash
   sudo mkdir -p /etc/ultrabalancer
   sudo cp config.toml /etc/ultrabalancer/
   ```

3. **Systemd Service**
   Create `/etc/systemd/system/ultrabalancer.service`

4. **Start Service**
   ```bash
   sudo systemctl enable ultrabalancer
   sudo systemctl start ultrabalancer
   ```

### Monitoring
- Check logs: `sudo journalctl -u ultrabalancer -f`
- Monitor metrics on port 9090

### Troubleshooting
Refer to docs/troubleshooting.md for common issues.
