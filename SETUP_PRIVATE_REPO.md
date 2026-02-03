# Setting Up Your Private Repository

The akmod package is ready. Follow these steps to create your private repository for testing.

## Option 1: GitHub (Recommended)

### Step 1: Create Private Repository on GitHub

1. Go to https://github.com/new
2. Repository name: `mt7927-bazzite` (or your preferred name)
3. Description: `MT7927 WiFi 7 driver for Bazzite (AMD RZ738)`
4. **Select "Private"**
5. Do NOT initialize with README (we have files already)
6. Click "Create repository"

### Step 2: Update Git Remote

```bash
cd "c:\Users\danme\New folder\mt7927"

# Remove the original upstream remote
git remote remove origin

# Add your private repository
git remote add origin https://github.com/YOUR_USERNAME/mt7927-bazzite.git

# Push to your private repo
git push -u origin main
```

### Step 3: Share with Your Tester

1. Go to your repository Settings → Collaborators
2. Click "Add people"
3. Enter your tester's GitHub username
4. They'll receive an invitation email

---

## Option 2: GitLab

### Create Private Repository

1. Go to https://gitlab.com/projects/new
2. Project name: `mt7927-bazzite`
3. Visibility level: **Private**
4. Click "Create project"

### Update Remote

```bash
git remote remove origin
git remote add origin https://gitlab.com/YOUR_USERNAME/mt7927-bazzite.git
git push -u origin main
```

---

## Testing Instructions for Your Tester

Send these instructions to your Bazzite tester:

### Prerequisites

- Bazzite installed on AMD system with MT7927/RZ738 WiFi
- `kernel-devel` installed: `rpm-ostree install kernel-devel`
- Git access to the private repository

### Clone and Test

```bash
# Clone the repository
git clone https://github.com/YOUR_USERNAME/mt7927-bazzite.git
cd mt7927-bazzite

# Run the build script
cd packaging
chmod +x build.sh
./build.sh

# Install for testing (temporary)
cd output
sudo ./install-test.sh
```

### Check Results

```bash
# View driver output
sudo dmesg | grep -E "mt7927|MT7927"

# Check if device is bound
lspci -k | grep -A3 "14c3:7927"
```

### Report Back

Please report:
1. Output of `dmesg | grep mt7927`
2. Whether WPDMA_GLO_CFG shows a value (not 0 or 0xffffffff)
3. Any errors or warnings

---

## What's Included

```
packaging/
├── driver/
│   └── mt7927.c         # Driver with corrected init sequence
├── akmod/
│   └── mt7927-kmod.spec # For permanent installation
├── firmware/
│   └── download-firmware.sh
├── build.sh             # One-click build
└── README.md            # Full documentation

mess/
├── discovery_summary.md    # Research findings
└── technical_reference.md  # Register reference
```

## Current Driver Status

The driver implements:
- ✅ Power management handoff (FW → Driver ownership)
- ✅ WFSYS reset sequence (unlocks registers)
- ✅ DMA initialization
- ⚠️ Firmware loading (framework ready, MCU protocol incomplete)

The key test is whether **WPDMA_GLO_CFG** becomes writable after the initialization sequence.
