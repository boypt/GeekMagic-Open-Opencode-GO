function opencodegoHandler() {
  return {
    loading: false,
    host: "",
    path: "",
    apiKey: "",
    maskedKey: "",
    apiKeyConfigured: false,
    verifyTls: true,
    saveMessage: "",
    saveSuccess: false,

    apiErrorMessage(status, fallback) {
      if (status === 401 || status === 403) {
        return "HTTP " + status + ": not authorized. Check the API Token page.";
      }
      if (status) {
        return "Request failed (HTTP " + status + ")";
      }
      return fallback;
    },

    fetchConfig() {
      apiFetch("/api/v1/opencodego/config")
        .then((r) => {
          if (!r.ok) {
            throw new Error(this.apiErrorMessage(r.status, "failed to load config"));
          }
          return r.json();
        })
        .then((data) => {
          this.host = data.opencodego_host || "";
          this.path = data.opencodego_path || "";
          this.maskedKey = data.opencodego_api_key || "";
          this.apiKeyConfigured = data.api_key_configured || false;
          this.verifyTls = (data.verify_tls_cert || 0) === 1;
        })
        .catch((err) => {
          console.error("failed to fetch opencodego config", err);
          this.saveSuccess = false;
          this.saveMessage = err.message || "Failed to load config";
        });
    },

    saveConfig() {
      this.loading = true;
      this.saveMessage = "";
      const payload = {
        opencodego_host: this.host,
        opencodego_path: this.path,
        verify_tls_cert: this.verifyTls ? 1 : 0,
      };
      // 只在用户输入了新 Key 时才提交，避免把打码值存回
      if (this.apiKey && this.apiKey.trim().length > 0) {
        payload.opencodego_api_key = this.apiKey.trim();
      }
      apiFetch("/api/v1/opencodego/config", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify(payload),
      })
        .then((r) =>
          r
            .json()
            .catch(() => null)
            .then((data) => ({ ok: r.ok, status: r.status, data })),
        )
        .then(({ ok, status, data }) => {
          if (ok && data && data.status === "ok") {
            this.saveSuccess = true;
            this.saveMessage = "Config saved";
            this.apiKey = "";
            this.fetchConfig();
          } else {
            this.saveSuccess = false;
            this.saveMessage =
              (data && data.message) || this.apiErrorMessage(status, "Save failed");
          }
        })
        .catch((err) => {
          this.saveSuccess = false;
          this.saveMessage = "Save failed: " + (err.message || "network error");
          console.error(err);
        })
        .finally(() => {
          this.loading = false;
        });
    },

    init() {
      this.fetchConfig();
    },
  };
}
