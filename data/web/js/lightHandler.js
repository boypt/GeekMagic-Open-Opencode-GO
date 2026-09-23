function lightHandler() {
  return {
    on: false,
    mode: "solid",
    r: 255,
    g: 140,
    b: 40,
    brightness: 60,
    statusMsg: "",
    loading: false,

    get colorHex() {
      return (
        "#" +
        [this.r, this.g, this.b]
          .map((v) => Number(v).toString(16).padStart(2, "0"))
          .join("")
      );
    },

    set colorHex(v) {
      const m = /^#?([0-9a-f]{6})$/i.exec(v || "");
      if (!m) return;
      const n = parseInt(m[1], 16);
      this.r = (n >> 16) & 0xff;
      this.g = (n >> 8) & 0xff;
      this.b = n & 0xff;
    },

    fetchLight() {
      apiFetch("/api/v1/light")
        .then((r) => r.json())
        .then((data) => {
          this.on = data.on === true;
          this.mode = data.mode || "solid";
          this.r = Number.isFinite(data.r) ? data.r : 255;
          this.g = Number.isFinite(data.g) ? data.g : 140;
          this.b = Number.isFinite(data.b) ? data.b : 40;
          this.brightness = Number.isFinite(data.brightness) ? data.brightness : 60;
        })
        .catch((err) => {
          this.statusMsg = "Failed to load light settings";
          console.error(err);
        });
    },

    applyLight() {
      this.loading = true;
      this.statusMsg = "";
      apiFetch("/api/v1/light", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({
          on: this.on,
          mode: this.mode,
          r: Number(this.r),
          g: Number(this.g),
          b: Number(this.b),
          brightness: Number(this.brightness),
        }),
      })
        .then((r) => r.json())
        .then((data) => {
          this.statusMsg = data.on !== undefined ? "Saved" : data.message || "Failed";
        })
        .catch((err) => {
          this.statusMsg = "Failed to save light settings";
          console.error(err);
        })
        .finally(() => {
          this.loading = false;
        });
    },

    init() {
      this.fetchLight();
    },
  };
}
