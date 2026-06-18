export class BrowserWindow {
  webContents = {
    send() {
      return undefined;
    },
    on() {
      return undefined;
    }
  };

  loadURL() {
    return Promise.resolve();
  }

  loadFile() {
    return Promise.resolve();
  }

  focus() {
    return undefined;
  }

  static getAllWindows() {
    return [];
  }
}

export const app = {
  isPackaged: false,
  getVersion() {
    return "0.0.0-test";
  },
  whenReady() {
    return Promise.resolve();
  },
  on() {
    return undefined;
  },
  quit() {
    return undefined;
  },
  getPath() {
    return "";
  },
  setAsDefaultProtocolClient() {
    return false;
  },
  requestSingleInstanceLock() {
    return true;
  }
};

export const ipcMain = {
  on() {
    return undefined;
  },
  handle() {
    return undefined;
  }
};

export const ipcRenderer = {
  sendSync() {
    return undefined;
  },
  invoke() {
    return Promise.resolve(undefined);
  },
  on() {
    return undefined;
  },
  off() {
    return undefined;
  }
};

export const contextBridge = {
  exposeInMainWorld() {
    return undefined;
  }
};

export const shell = {
  openExternal() {
    return Promise.resolve();
  }
};

export const safeStorage = {
  isEncryptionAvailable() {
    return false;
  },
  encryptString(value: string) {
    return new TextEncoder().encode(value);
  },
  decryptString(value: Uint8Array) {
    return new TextDecoder().decode(value);
  }
};
