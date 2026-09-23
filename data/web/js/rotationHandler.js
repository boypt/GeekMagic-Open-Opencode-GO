function rotationHandler() {
  return {
    loading: false,
    loadingRotation: false,
    loadingMirror: false,
    loadingPanel: false,
    loadingBrightness: false,
    rotation: 0,
    mirrorX: false,
    mirrorY: false,
    lcdBgr: false,
    lcdInitSd2: false,
    lcdBrightness: 78,
    statusMsg: "",
    statusRotation: "",
    statusMirror: "",
    statusPanel: "",
    statusBrightness: "",

    // 一次拉全部显示设置（GET /display/rotation 返回全部字段）
    fetchAll(manual) {
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
          this.statusMsg = manual ? "Settings refreshed" : "";
        })
        .catch((err) => {
          this.statusMsg = "Failed to load display settings";
          console.error(err);
        })
        .finally(() => {
          this.loading = false;
        });
    },

    // 每个区域的保存按钮只提交自己区域的字段（API 支持部分更新）
    saveRotation() {
      this.loadingRotation = true;
      this.statusRotation = "";
      const payload = { rotation: Number(this.rotation) };

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
            this.statusRotation = "Rotation saved";
          } else {
            this.statusRotation = data.message || "Failed to save rotation";
          }
        })
        .catch((err) => {
          this.statusRotation = "Failed to save rotation";
          console.error(err);
        })
        .finally(() => {
          this.loadingRotation = false;
        });
    },

    saveMirror() {
      this.loadingMirror = true;
      this.statusMirror = "";
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
            this.statusMirror = "Mirror saved";
          } else {
            this.statusMirror = data.message || "Failed to save mirror";
          }
        })
        .catch((err) => {
          this.statusMirror = "Failed to save mirror";
          console.error(err);
        })
        .finally(() => {
          this.loadingMirror = false;
        });
    },

    savePanel() {
      this.loadingPanel = true;
      this.statusPanel = "";
      const payload = {
        lcd_bgr: this.lcdBgr === true,
        lcd_init_sd2: this.lcdInitSd2 === true,
      };

      // /display/mirror 端点接受 panel profile 字段的部分更新
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
            this.statusPanel = "Panel color/init saved";
          } else {
            this.statusPanel = data.message || "Failed to save panel settings";
          }
        })
        .catch((err) => {
          this.statusPanel = "Failed to save panel settings";
          console.error(err);
        })
        .finally(() => {
          this.loadingPanel = false;
        });
    },

    // 松手即保存并生效（PWM）
    saveBrightness() {
      this.loadingBrightness = true;
      this.statusBrightness = "";
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
            this.statusBrightness = "Brightness saved";
          } else {
            this.statusBrightness = data.message || "Failed to save brightness";
          }
        })
        .catch((err) => {
          this.statusBrightness = "Failed to save brightness";
          console.error(err);
        })
        .finally(() => {
          this.loadingBrightness = false;
        });
    },

    // 松手前的本地预览：仅更新数字显示，亮度本身等 change 时保存
    previewBrightness() {
      this.lcdBrightness = Math.min(100, Math.max(1, Number(this.lcdBrightness) || 78));
    },

    init() {
      this.fetchAll(false);
    },
  };
}
