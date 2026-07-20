#!/usr/bin/env bash
# tutuicmptunnel-kmod server one-click installer
# Usage: curl -fsSL <url>/install-server.sh | sudo bash
#
# Supports: Ubuntu (≥20.04), Arch Linux
# What it does:
#   1. Install build dependencies
#   2. Clone, compile, and install tutuicmptunnel-kmod
#   3. Build and install the kernel module via DKMS
#   4. Generate a random PSK for tuctl_server
#   5. Install and enable systemd services (kmod-server + tuctl_server)

set -euo pipefail

# ── Configuration (override via environment) ──────────────────────────
REPO_URL="${TUTU_REPO_URL:-https://github.com/hrimfaxi/tutuicmptunnel-kmod}"
INSTALL_DIR="${TUTU_INSTALL_DIR:-/opt/tutuicmptunnel-kmod}"
SERVER_PORT="${TUTU_SERVER_PORT:-14801}"
MEMLIMIT="${TUTU_MEMLIMIT:-1048576}"  # 1MB default
NET_IFACE="${TUTU_NET_IFACE:-}"       # auto-detect if empty

# ── Helpers ───────────────────────────────────────────────────────────
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

info()  { echo -e "${GREEN}[INFO]${NC} $*"; }
warn()  { echo -e "${YELLOW}[WARN]${NC} $*"; }
error() { echo -e "${RED}[ERROR]${NC} $*" >&2; }

die() {
  error "$@"
  exit 1
}

check_root() {
  [[ $EUID -eq 0 ]] || die "This script must be run as root (or with sudo)."
}

detect_distro() {
  if command -v apt-get &>/dev/null; then
    echo "debian"
  elif command -v pacman &>/dev/null; then
    echo "arch"
  else
    die "Unsupported distribution. Only Ubuntu/Debian and Arch Linux are supported."
  fi
}

detect_iface() {
  if [[ -n "$NET_IFACE" ]]; then
    echo "$NET_IFACE"
    return
  fi
  # Pick the default route interface
  local iface
  iface=$(ip -o route show default 2>/dev/null | awk '{print $5}' | head -1)
  if [[ -z "$iface" ]]; then
    die "Cannot auto-detect network interface. Set TUTU_NET_IFACE manually."
  fi
  echo "$iface"
}

generate_psk() {
  # Generate a 32-byte random hex string (64 chars)
  if command -v openssl &>/dev/null; then
    openssl rand -hex 32
  elif [[ -r /dev/urandom ]]; then
    head -c 32 /dev/urandom | xxd -p -c 64
  else
    die "Cannot generate random PSK (no openssl or /dev/urandom)."
  fi
}

# ── Step 1: Install dependencies ─────────────────────────────────────
install_deps() {
  local distro="$1"
  info "Installing build dependencies for ${distro}..."
  case "$distro" in
    debian)
      export DEBIAN_FRONTEND=noninteractive
      apt-get update -qq
      apt-get install -y -qq \
        git libsodium-dev dkms build-essential \
        "linux-headers-$(uname -r)" flex bison libmnl-dev cmake pkg-config \
        xxd
      ;;
    arch)
      pacman -Sy --noconfirm --needed \
        git libsodium dkms base-devel linux-headers \
        flex bison libmnl cmake pkg-config
      ;;
  esac
}

# ── Step 2: Clone & build ────────────────────────────────────────────
build_project() {
  if [[ -d "$INSTALL_DIR/.git" ]]; then
    info "Updating existing repo at ${INSTALL_DIR}..."
    git -C "$INSTALL_DIR" pull --ff-only || true
  else
    info "Cloning ${REPO_URL} into ${INSTALL_DIR}..."
    rm -rf "$INSTALL_DIR"
    git clone --depth=1 "$REPO_URL" "$INSTALL_DIR"
  fi

  info "Building tutuicmptunnel-kmod..."
  cmake -DCMAKE_BUILD_TYPE=Release -DENABLE_HARDEN_MODE=1 -B "$INSTALL_DIR/build" -S "$INSTALL_DIR"
  make -C "$INSTALL_DIR/build" -j"$(nproc)"
  make -C "$INSTALL_DIR/build" install
}

# ── Step 3: Install kernel module ────────────────────────────────────
install_kmod() {
  info "Installing kernel module via DKMS..."
  cd "$INSTALL_DIR/kmod"

  # Remove old version if present
  local old_ver
  old_ver=$(dkms status tutuicmptunnel 2>/dev/null | head -1 | grep -oP '\d+\.\d+' || true)
  if [[ -n "$old_ver" ]]; then
    info "Removing previous DKMS version ${old_ver}..."
    dkms remove tutuicmptunnel/"$old_ver" --all 2>/dev/null || true
  fi

  make dkms

  # Auto-load on boot
  if ! grep -q '^tutuicmptunnel$' /etc/modules-load.d/modules.conf 2>/dev/null; then
    mkdir -p /etc/modules-load.d
    echo 'tutuicmptunnel' >> /etc/modules-load.d/modules.conf
  fi

  modprobe tutuicmptunnel || warn "modprobe failed (may need reboot)"
}

# ── Step 4: Install systemd services ─────────────────────────────────
install_services() {
  local psk="$1"
  local iface="$2"

  info "Installing systemd service files..."

  # kmod server service
  cp "$INSTALL_DIR/contrib/etc/systemd/system/tutuicmptunnel-kmod-server@.service" \
     /etc/systemd/system/

  # tuctl_server service (with generated PSK and memlimit)
  cat > /etc/systemd/system/tutuicmptunnel-tuctl-server.service <<EOF
[Unit]
Description=tutuicmptunnel tuctl server
After=network.target

[Service]
Type=simple
Environment="TUTUICMPTUNNEL_PWHASH_MEMLIMIT=${MEMLIMIT}"
ExecStart=/usr/local/bin/tuctl_server --psk ${psk} --port ${SERVER_PORT}
Restart=on-failure
RestartSec=5

[Install]
WantedBy=multi-user.target
EOF

  info "Enabling and starting services..."
  systemctl daemon-reload

  # Enable kmod server
  systemctl enable --now "tutuicmptunnel-kmod-server@${iface}.service"

  # Enable tuctl_server
  systemctl enable --now tutuicmptunnel-tuctl-server.service
}

# ── Step 5: Print summary ────────────────────────────────────────────
print_summary() {
  local psk="$1"
  local iface="$2"

  echo ""
  echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
  info "Installation complete!"
  echo ""
  echo "  Network interface : ${iface}"
  echo "  tuctl_server port : ${SERVER_PORT}"
  echo "  PSK               : ${psk}"
  echo "  Memlimit          : ${MEMLIMIT} bytes"
  echo ""
  echo "  ⚠️  Save the PSK above — clients need it to connect."
  echo ""
  echo "  Verify with:"
  echo "    sudo ktuctl"
  echo "    systemctl status tutuicmptunnel-tuctl-server"
  echo ""
  echo "  NTP must be synchronized (timestamp verification):"
  echo "    timedatectl | grep 'System clock synchronized'"
  echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
}

# ── Main ──────────────────────────────────────────────────────────────
main() {
  check_root

  local distro iface psk

  distro=$(detect_distro)
  info "Detected distro: ${distro}"

  iface=$(detect_iface)
  info "Network interface: ${iface}"

  psk=$(generate_psk)

  install_deps "$distro"
  build_project
  install_kmod
  install_services "$psk" "$iface"
  print_summary "$psk" "$iface"
}

main "$@"
