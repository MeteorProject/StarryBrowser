// 设、删 localStorage、sessionStorage 里的项（Cookie 横幅、弹窗「看过了」的记号）。
// @scriptlet set-local-storage-item setLocalStorageItem
// @scriptlet set-session-storage-item setSessionStorageItem
// @scriptlet trusted-set-local-storage-item trustedSetLocalStorageItem trusted
// @scriptlet trusted-set-session-storage-item trustedSetSessionStorageItem trusted

const safeStorageValues = new Set([
  '', 'undefined', 'null', 'false', 'true', 'on', 'off', 'yes', 'no', 'y', 'n', 'ok',
  'accept', 'accepted', 'reject', 'rejected', 'allow', 'allowed', 'deny', 'denied',
  'forbidden', 'forever', 'decline', 'declined', 'dismiss', 'dismissed', 'essential',
  'necessary', 'required', 'granted', 'enabled', 'disabled', 'done', 'closed', 'hide',
  'hidden', '[]', '{}',
]);

// value 是 `$remove$` 时删掉（key 可以是 /正则/，删掉所有对上的）。
function setStorageItem(which, trusted, key, value) {
  if (key === '') {
    return;
  }
  let storage;
  try {
    storage = $.window[which];
  } catch (e) {
    return;
  }
  if (!isObject(storage)) {
    return;
  }
  try {
    if (value === '$remove$') {
      if (/^\/.+\/[a-z]*$/.test(key)) {
        const re = toRegex(key);
        for (const name of $.keys(storage)) {
          if (reTest(re, name)) {
            storage.removeItem(name);
          }
        }
      } else {
        storage.removeItem(key);
      }
      return;
    }
    if (trusted) {
      value = expandTimeTokens(value);
    } else {
      // `'"no"'` 这种带引号的（JSON 的字符串）也认。
      const quoted = /^"(.*)"$/.exec(value);
      const bare = quoted !== null ? quoted[1] : value;
      const lower = bare.toLowerCase();
      if (lower === 'emptyarr') {
        value = '[]';
      } else if (lower === 'emptyobj') {
        value = '{}';
      } else if (!safeStorageValues.has(lower) &&
                 !(/^\d+$/.test(bare) && Number(bare) <= 0x7fff)) {
        return;
      }
    }
    if (storage.getItem(key) !== value) {
      storage.setItem(key, value);
    }
  } catch (e) {
  }
}

function setLocalStorageItem(key = '', value = '') {
  setStorageItem('localStorage', false, key, value);
}

function setSessionStorageItem(key = '', value = '') {
  setStorageItem('sessionStorage', false, key, value);
}

function trustedSetLocalStorageItem(key = '', value = '') {
  setStorageItem('localStorage', true, key, value);
}

function trustedSetSessionStorageItem(key = '', value = '') {
  setStorageItem('sessionStorage', true, key, value);
}
