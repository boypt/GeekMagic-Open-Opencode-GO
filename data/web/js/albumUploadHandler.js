function albumUploadHandler() {
  return {
    uploading: false,
    uploadMessage: "",
    images: [],
    usedBytes: 0,
    totalBytes: 0,
    freeBytes: 0,
    listLoaded: false,

    get usedBytesHR() {
      return humanFileSize(this.usedBytes);
    },

    get totalBytesHR() {
      return humanFileSize(this.totalBytes);
    },

    get freeBytesHR() {
      return humanFileSize(this.freeBytes);
    },

    // 浏览器端把任意图片转成 240x240 RGB565(LE) 原始位图（cover 裁剪），
    // 固件零解码器，直接流式送屏
    async convertToRgb565(file) {
      const bitmap = await createImageBitmap(file);
      const canvas = document.createElement("canvas");
      canvas.width = 240;
      canvas.height = 240;
      const ctx = canvas.getContext("2d");

      const scale = Math.max(240 / bitmap.width, 240 / bitmap.height);
      const w = bitmap.width * scale;
      const h = bitmap.height * scale;
      ctx.drawImage(bitmap, (240 - w) / 2, (240 - h) / 2, w, h);

      const data = ctx.getImageData(0, 0, 240, 240).data;
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

    async uploadImage() {
      this.uploading = true;
      this.uploadMessage = "";
      const file = this.$refs.fileInput.files[0];

      if (!file) {
        this.uploadMessage = "Please select an image";
        this.uploading = false;
        return;
      }

      try {
        const rgb565 = await this.convertToRgb565(file);
        const name =
          (file.name.replace(/\.[^.]+$/, "") || "photo").replace(/[^\w.-]/g, "_") +
          ".rgb565";

        const formData = new FormData();
        formData.append(
          "upload",
          new Blob([rgb565], { type: "application/octet-stream" }),
          name
        );

        const response = await apiFetch("/api/v1/album", {
          method: "POST",
          body: formData,
        });
        const result = await response.json();

        if (result.status === "success") {
          this.uploadMessage = "Uploaded: " + result.filename;
          await this.fetchList();
        } else {
          this.uploadMessage = result.message || "Upload failed";
        }
      } catch (e) {
        this.uploadMessage = "Error: " + e;
      }

      this.uploading = false;
    },

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
