function ntpHandler() {
  return {
    loading: false,
    lastStatus: "",
    lastSyncTime: 0,
    lastOk: false,
    lastServer: "",
    ntpServer: "",

    fetchStatus() {
      apiFetch("/api/v1/ntp/status")
        .then((r) => r.json())
        .then((data) => {
          this.lastStatus = data.lastStatus || "";
          this.lastSyncTime = data.lastSyncTime || 0;
          this.lastOk = data.lastOk || false;
          this.lastServer = data.ntp_server || "";
        })
        .catch((err) => {
          this.lastStatus = "error fetching status";
          console.error(err);
        });
    },

    syncNow() {
      this.loading = true;
      apiFetch("/api/v1/ntp/sync", { method: "POST" })
        .then((r) => r.json())
        .then((data) => {
          this.lastStatus = data.lastStatus || "";
          this.lastSyncTime = data.lastSyncTime || 0;
          this.lastOk = data.status === "ok";
          this.lastServer = data.ntp_server || "";
        })
        .catch((err) => {
          this.lastStatus = "sync failed";
          console.error(err);
        })
        .finally(() => {
          this.loading = false;
        });
    },

    // 状态文案：synced 时拼上实际生效的服务器地址（例 "Synced: ... to pool.ntp.org"）
    statusText() {
      return this.lastServer
        ? this.lastStatus + " to " + this.lastServer
        : this.lastStatus;
    },

    fetchConfig() {
      apiFetch("/api/v1/ntp/config")
        .then((r) => r.json())
        .then((data) => {
          this.ntpServer = data.ntp_server || "";
        })
        .catch((err) => {
          console.error("failed to fetch ntp config", err);
        });
    },

    saveConfig() {
      const payload = { ntp_server: this.ntpServer };
      apiFetch("/api/v1/ntp/config", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify(payload),
      })
        .then((r) => r.json())
        .then((data) => {
          if (data.status === "ok") {
            this.lastStatus = "Config saved, sync triggered";
            this.fetchStatus();
          } else {
            this.lastStatus = data.message || "save failed";
          }
        })
        .catch((err) => {
          this.lastStatus = "save failed";
          console.error(err);
        });
    },

    humanTime(ts) {
      if (!ts || ts === 0) return "never";
      const d = new Date(ts * 1000);
      return d.toLocaleString();
    },

    init() {
      this.fetchStatus();
      this.fetchConfig();
    },
  };
}
