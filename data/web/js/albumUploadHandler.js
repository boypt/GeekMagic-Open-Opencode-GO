function albumUploadHandler() {
  return {
    uploading: false,
    uploadMessage: "",
    images: [],
    usedBytes: 0,
    totalBytes: 0,
    freeBytes: 0,
    listLoaded: false,

    // 裁剪控件状态（Cropper.js，单图模式）
    cropping: false,
    cropper: null,
    pendingFile: null,
    pendingCanvas: null,

    get usedBytesHR() {
      return humanFileSize(this.usedBytes);
    },

    get totalBytesHR() {
      return humanFileSize(this.totalBytes);
    },

    get freeBytesHR() {
      return humanFileSize(this.freeBytes);
    },

    // ---- 解码：JPEG 走 jpeg-js（CDN），其余走浏览器内建 ----
    // 浏览器内建解码对 CMYK/YCCK JPG 会反色（蓝→黄），jpeg-js 显式处理
    async ensureJpegDecoder() {
      if (window.__jpegJsDecode) return window.__jpegJsDecode;
      const urls = [
        "https://cdn.jsdelivr.net/npm/jpeg-js@0.4.4/+esm",
        "https://esm.sh/jpeg-js@0.4.4",
      ];
      for (const u of urls) {
        try {
          const mod = await import(u);
          const decode = mod.decode || (mod.default && mod.default.decode);
          if (decode) {
            window.__jpegJsDecode = decode;
            return decode;
          }
        } catch (e) {
          /* 试下一个源 */
        }
      }
      return null;
    },

    async decodeToCanvas(file) {
      const buf = await file.arrayBuffer();
      const u8 = new Uint8Array(buf);
      let src = null;

      if (u8.length > 3 && u8[0] === 0xff && u8[1] === 0xd8) {
        try {
          const decode = await this.ensureJpegDecoder();
          if (decode) {
            const img = decode(u8, {
              useTArray: true,
              formatAsRGBA: true,
              tolerantDecoding: true,
            });
            src = document.createElement("canvas");
            src.width = img.width;
            src.height = img.height;
            const clamped = new Uint8ClampedArray(
              img.data.buffer,
              img.data.byteOffset,
              img.data.length
            );
            src.getContext("2d").putImageData(
              new ImageData(clamped, img.width, img.height),
              0,
              0
            );
          }
        } catch (e) {
          console.warn("jpeg-js decode failed, fallback to browser:", e);
        }
      }

      if (!src) {
        const bitmap = await createImageBitmap(file);
        src = document.createElement("canvas");
        src.width = bitmap.width;
        src.height = bitmap.height;
        src.getContext("2d").drawImage(bitmap, 0, 0);
      }

      return src;
    },

    // 240x240 画布 -> RGB565(LE) 字节
    canvasToRgb565(canvas) {
      const data = canvas.getContext("2d").getImageData(0, 0, 240, 240).data;
      const out = new Uint8Array(240 * 240 * 2);

      for (let i = 0, j = 0; i < data.length; i += 4, j += 2) {
        const v =
          ((data[i] & 0xf8) << 8) |
          ((data[i + 1] & 0xfc) << 3) |
          (data[i + 2] >> 3);
        out[j] = v & 0xff;
        out[j + 1] = v >> 8;
      }

      return out;
    },

    // 批量模式的自动适配：cover 裁剪缩放到 240x240
    coverTo240(src) {
      const canvas = document.createElement("canvas");
      canvas.width = 240;
      canvas.height = 240;
      const ctx = canvas.getContext("2d");
      const scale = Math.max(240 / src.width, 240 / src.height);
      const w = src.width * scale;
      const h = src.height * scale;
      ctx.imageSmoothingQuality = "high";
      ctx.drawImage(src, (240 - w) / 2, (240 - h) / 2, w, h);
      return canvas;
    },

    imageName(file) {
      return (
        (file.name.replace(/\.[^.]+$/, "") || "photo").replace(
          /[^\w.-]/g,
          "_"
        ) + ".rgb565"
      );
    },

    async pushCanvas(canvas, file) {
      const rgb565 = this.canvasToRgb565(canvas);
      const formData = new FormData();
      formData.append(
        "upload",
        new Blob([rgb565], { type: "application/octet-stream" }),
        this.imageName(file)
      );

      const response = await apiFetch("/api/v1/album", {
        method: "POST",
        body: formData,
      });
      const result = await response.json();

      if (result.status !== "success") {
        throw new Error(result.message || "upload failed");
      }
    },

    // ---- 上传入口：单张进裁剪控件，多张批量自动适配 ----
    async uploadImage() {
      const input = this.$refs.fileInput;
      const files = Array.from(input.files || []);

      if (files.length === 0) {
        this.uploadMessage = "Please select image(s)";
        return;
      }

      if (files.length === 1) {
        await this.openCropper(files[0]);
        return;
      }

      this.uploading = true;
      this.uploadMessage = "";
      let ok = 0;
      const failed = [];

      for (let i = 0; i < files.length; i++) {
        const file = files[i];
        this.uploadMessage = `Uploading ${i + 1}/${files.length}: ${file.name}`;

        try {
          const src = await this.decodeToCanvas(file);
          await this.pushCanvas(this.coverTo240(src), file);
          ok++;
        } catch (e) {
          failed.push(file.name);
        }
      }

      this.uploadMessage =
        failed.length === 0
          ? `Uploaded ${ok}/${files.length}`
          : `Uploaded ${ok}/${files.length}, failed: ${failed.join(", ")}`;

      input.value = "";
      await this.fetchList();
      this.uploading = false;
    },

    // ---- Cropper.js 交互裁剪（单图）----
    async openCropper(file) {
      try {
        this.pendingFile = file;
        this.pendingCanvas = await this.decodeToCanvas(file);
        this.cropping = true;

        await this.$nextTick();
        const img = this.$refs.cropImage;
        if (!img) throw new Error("crop UI not ready");
        img.src = this.pendingCanvas.toDataURL("image/jpeg", 0.92);
        this.cropper = new Cropper(img, {
          aspectRatio: 1,
          viewMode: 1,
          autoCropArea: 1,
          movable: true,
          zoomable: true,
          scalable: true,
          rotatable: true,
          background: false,
        });
      } catch (e) {
        this.uploadMessage = "Error: " + e;
        this.closeCropper();
      }
    },

    rotateCrop(delta) {
      if (this.cropper) this.cropper.rotate(delta);
    },

    async cropUpload() {
      if (!this.cropper) return;
      this.uploading = true;

      try {
        const canvas = this.cropper.getCroppedCanvas({
          width: 240,
          height: 240,
          imageSmoothingEnabled: true,
          imageSmoothingQuality: "high",
        });
        await this.pushCanvas(canvas, this.pendingFile);
        this.uploadMessage = "Uploaded: " + this.imageName(this.pendingFile);
        this.closeCropper();
        await this.fetchList();
      } catch (e) {
        this.uploadMessage = "Error: " + e;
      }

      this.uploading = false;
    },

    cropAuto() {
      if (!this.pendingCanvas) return;
      this.uploading = true;
      this.pushCanvas(this.coverTo240(this.pendingCanvas), this.pendingFile)
        .then(async () => {
          this.uploadMessage = "Uploaded: " + this.imageName(this.pendingFile);
          this.closeCropper();
          await this.fetchList();
        })
        .catch((e) => {
          this.uploadMessage = "Error: " + e;
        })
        .finally(() => {
          this.uploading = false;
        });
    },

    closeCropper() {
      if (this.cropper) {
        this.cropper.destroy();
        this.cropper = null;
      }
      this.cropping = false;
      this.pendingFile = null;
      this.pendingCanvas = null;
      if (this.$refs.fileInput) this.$refs.fileInput.value = "";
    },

    // ---- 列表 / 场景 ----
    async fetchList() {
      this.listLoaded = false;

      try {
        const response = await apiFetch("/api/v1/album");
        const data = await response.json();
        this.images = data.files || [];
        this.usedBytes = data.usedBytes || 0;
        this.totalBytes = data.totalBytes || 0;
        this.freeBytes = data.freeBytes || 0;
      } catch (e) {
        this.images = [];
        this.usedBytes = this.totalBytes = this.freeBytes = 0;
      }

      this.listLoaded = true;
    },

    async switchScene(body, okWord) {
      const res = await apiFetch("/api/v1/scene", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify(body),
      });
      const data = await res.json().catch(() => ({}));

      if (data.status !== okWord) {
        alert("Failed: " + (data.message || res.status));
      }
    },

    playImage(name) {
      this.switchScene({ scene: "album", param: name }, "ok");
    },

    playAll() {
      this.switchScene({ scene: "album" }, "ok");
    },

    stopPlayback() {
      this.switchScene({ scene: "balance" }, "ok");
    },

    async deleteImage(name) {
      if (!confirm(`Delete ${name}? This cannot be undone.`)) return;

      try {
        const res = await apiFetch("/api/v1/album", {
          method: "DELETE",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify({ name: name }),
        });

        if (!res.ok) {
          alert("Delete failed: " + res.status);
          return;
        }

        await this.fetchList();
      } catch (e) {
        alert("Error deleting image: " + e);
      }
    },

    init() {
      this.fetchList();
    },
  };
}
