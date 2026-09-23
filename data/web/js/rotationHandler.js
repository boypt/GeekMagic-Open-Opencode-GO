function rotationHandler() {
  return {
    loading: false,
    rotation: 0,
    mirrorX: false,
    mirrorY: false,
    lcdBgr: false,
    lcdInitSd2: false,
    lcdBrightness: 78,
    statusMsg: "",

    fetchRotation() {
      this.loading = true;
      apiFetch("/api/v1/display/rotation")
        .then((r) => r.json())
        .then((data) => {
          this.rotation = Number.isInteger(data.rotation) ? data.rotation : 0;
          this.mirrorX = data.lcd_mirror_x === true;
          this.mirrorY = data.lcd_mirror_y === true;
          this.lcdBgr = data.lcd_bgr === true;
          this.lcdInitSd2 = data.lcd_init_sd2 === true;
          if (Number.isFinite(data.lcd_brightness)) {
            this.lcdBrightness = Math.min(100, Math.max(1, data.lcd_brightness));
          }
          this.statusMsg = "";
        })
        .catch((err) => {
          this.statusMsg = "Failed to load rotation";
          console.error(err);
        })
        .finally(() => {
          this.loading = false;
        });
    },

    saveRotation() {
      this.loading = true;
      const payload = {
        rotation: Number(this.rotation),
        lcd_mirror_x: this.mirrorX === true,
        lcd_mirror_y: this.mirrorY === true,
        lcd_bgr: this.lcdBgr === true,
        lcd_init_sd2: this.lcdInitSd2 === true,
      };

      apiFetch("/api/v1/display/rotation", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify(payload),
      })
        .then((r) => r.json())
        .then((data) => {
          if (data.status === "ok") {
            this.rotation = Number.isInteger(data.rotation)
              ? data.rotation
              : payload.rotation;
            this.mirrorX = data.lcd_mirror_x === true;
            this.mirrorY = data.lcd_mirror_y === true;
            this.lcdBgr = data.lcd_bgr === true;
            this.lcdInitSd2 = data.lcd_init_sd2 === true;
            this.statusMsg = "Rotation updated";
          } else {
            this.statusMsg = data.message || "Failed to save rotation";
          }
        })
        .catch((err) => {
          this.statusMsg = "Failed to save rotation";
          console.error(err);
        })
        .finally(() => {
          this.loading = false;
        });
    },

    saveMirror() {
      this.loading = true;
      const payload = {
        lcd_mirror_x: this.mirrorX === true,
        lcd_mirror_y: this.mirrorY === true,
      };

      apiFetch("/api/v1/display/mirror", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify(payload),
      })
        .then((r) => r.json())
        .then((data) => {
          if (data.status === "ok") {
            this.mirrorX = data.lcd_mirror_x === true;
            this.mirrorY = data.lcd_mirror_y === true;
            this.statusMsg = "Mirror updated";
          } else {
            this.statusMsg = data.message || "Failed to save mirror";
          }
        })
        .catch((err) => {
          this.statusMsg = "Failed to save mirror";
          console.error(err);
        })
        .finally(() => {
          this.loading = false;
        });
    },

    savePanel() {
      this.loading = true;
      const payload = {
        lcd_bgr: this.lcdBgr === true,
        lcd_init_sd2: this.lcdInitSd2 === true,
      };

      apiFetch("/api/v1/display/mirror", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify(payload),
      })
        .then((r) => r.json())
        .then((data) => {
          if (data.status === "ok") {
            this.lcdBgr = data.lcd_bgr === true;
            this.lcdInitSd2 = data.lcd_init_sd2 === true;
            this.statusMsg = "Panel color/init updated";
          } else {
            this.statusMsg = data.message || "Failed to save panel settings";
          }
        })
        .catch((err) => {
          this.statusMsg = "Failed to save panel settings";
          console.error(err);
        })
        .finally(() => {
          this.loading = false;
        });
    },

    saveBrightness() {
      this.loading = true;
      const payload = {
        lcd_brightness: Math.min(100, Math.max(1, Number(this.lcdBrightness) || 78)),
      };

      apiFetch("/api/v1/display/brightness", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify(payload),
      })
        .then((r) => r.json())
        .then((data) => {
          if (data.status === "ok") {
            this.lcdBrightness = data.lcd_brightness;
            this.statusMsg = "Brightness updated";
          } else {
            this.statusMsg = data.message || "Failed to save brightness";
          }
        })
        .catch((err) => {
          this.statusMsg = "Failed to save brightness";
          console.error(err);
        })
        .finally(() => {
          this.loading = false;
        });
    },

    // 松手前的本地预览：仅更新数字显示，亮度本身等 change 时保存
    previewBrightness() {
      this.lcdBrightness = Math.min(100, Math.max(1, Number(this.lcdBrightness) || 78));
    },

    init() {
      this.fetchRotation();
    },
  };
}
