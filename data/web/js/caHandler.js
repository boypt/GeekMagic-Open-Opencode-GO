function caHandler() {
  return {
    loading: false,
    saving: false,
    deleting: false,
    source: "",
    bytes: 0,
    pem: "",
    statusMessage: "",
    statusSuccess: false,
    confirmDelete: false,

    apiErrorMessage(status, fallback) {
      if (status === 401 || status === 403) {
        return (
          "HTTP " + status + ": not authorized. Check the API Token page."
        );
      }
      if (status) {
        return "Request failed (HTTP " + status + ")";
      }
      return fallback;
    },

    fetchCa() {
      this.loading = true;
      this.statusMessage = "";
      apiFetch("/api/v1/opencodego/ca")
        .then((r) => {
          if (!r.ok) {
            throw new Error(
              this.apiErrorMessage(r.status, "failed to load CA state"),
            );
          }
          return r.json();
        })
        .then((data) => {
          this.source = data.source || "";
          this.bytes = data.bytes || 0;
          this.pem = data.pem || "";
          if (this.source === "custom") {
            this.statusSuccess = true;
            this.statusMessage =
              "Custom root CA loaded (" + this.bytes + " bytes)";
          } else if (this.source === "none") {
            this.statusSuccess = true;
            this.statusMessage = "Built-in root CA in use";
          }
        })
        .catch((err) => {
          console.error("failed to fetch opencodego ca", err);
          this.statusSuccess = false;
          this.statusMessage = err.message || "Failed to load CA state";
        })
        .finally(() => {
          this.loading = false;
        });
    },

    savePem() {
      this.saving = true;
      this.statusMessage = "";
      const pem = this.pem.trim();
      apiFetch("/api/v1/opencodego/ca", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ pem: pem }),
      })
        .then((r) =>
          r
            .json()
            .catch(() => null)
            .then((data) => ({ ok: r.ok, status: r.status, data })),
        )
        .then(({ ok, status, data }) => {
          if (ok && data && data.status === "ok") {
            this.statusSuccess = true;
            this.statusMessage =
              "Custom CA saved (" + data.bytes + " bytes)";
            this.fetchCa();
          } else {
            this.statusSuccess = false;
            this.statusMessage =
              (data && data.message) || this.apiErrorMessage(status, "Save failed");
          }
        })
        .catch((err) => {
          this.statusSuccess = false;
          this.statusMessage = "Save failed: " + (err.message || "network error");
          console.error(err);
        })
        .finally(() => {
          this.saving = false;
        });
    },

    deleteCa() {
      if (!this.confirmDelete) {
        this.confirmDelete = true;
        return;
      }
      this.deleting = true;
      this.confirmDelete = false;
      this.statusMessage = "";
      apiFetch("/api/v1/opencodego/ca", { method: "DELETE" })
        .then((r) =>
          r
            .json()
            .catch(() => null)
            .then((data) => ({ ok: r.ok, status: r.status, data })),
        )
        .then(({ ok, status, data }) => {
          if (ok && data && data.status === "ok") {
            this.statusSuccess = true;
            this.statusMessage = "Custom CA deleted, built-in CA restored";
            this.fetchCa();
          } else {
            this.statusSuccess = false;
            this.statusMessage = this.apiErrorMessage(status, "Delete failed");
          }
        })
        .catch((err) => {
          this.statusSuccess = false;
          this.statusMessage =
            "Delete failed: " + (err.message || "network error");
          console.error(err);
        })
        .finally(() => {
          this.deleting = false;
        });
    },

    init() {
      this.fetchCa();
    },
  };
}
