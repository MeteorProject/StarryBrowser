// 把某个属性固定成一个值（网页判断「有没有拦截广告」、广告的开关常用的那些）。
// @scriptlet set-constant setConstant
// @alias set set-constant
// @alias override-property-read set-constant
// @scriptlet trusted-set-constant trustedSetConstant trusted
// @alias trusted-set trusted-set-constant

function setConstant(chain = '', raw = '', ...rest) {
  setConstantValue(false, chain, raw, rest);
}

// 能设成任意的值（文字、JSON）。
function trustedSetConstant(chain = '', raw = '', ...rest) {
  setConstantValue(true, chain, raw, rest);
}

function setConstantValue(trusted, chain, raw, rest) {
  if (chain === '') {
    return;
  }
  const parsed = constantValue(raw, trusted);
  if (parsed === undefined) {
    return;
  }
  let value = parsed[0];
  const options = namedArgs(rest);
  const plain = value;
  switch (options.as) {
    case 'function':
      value = function() {
        return plain;
      };
      break;
    case 'callback':
      value = function() {
        return function() {
          return plain;
        };
      };
      break;
    case 'resolved':
      value = $.Promise.resolve(plain);
      break;
    case 'rejected':
      value = $.Promise.reject(plain);
      value.catch(() => {});
      break;
  }
  // 原来有值、类型和要设的不一样时不管它（多半是网站改了，硬设会弄坏页面）；网页之后赋了
  // 别的类型的值也就让给网页。信得过的列表不管这些。
  const conflicts = current => !trusted && current !== undefined && current !== null &&
      value !== undefined && value !== null && typeof current !== typeof value;
  let yielded = false;
  let pageValue;
  trapChain($.window, chain, (owner, prop) => {
    const access = propertyAccess(owner, prop);
    let current;
    try {
      current = owner[prop];
    } catch (e) {
    }
    if (conflicts(current) || !access.configurable) {
      return;
    }
    try {
      $.defineProperty(owner, prop, {
        configurable: false,
        enumerable: access.enumerable,
        get() {
          try {
            access.get(this);
          } catch (e) {
          }
          return yielded ? pageValue : value;
        },
        set(next) {
          if (conflicts(next)) {
            yielded = true;
            pageValue = next;
          }
        },
      });
    } catch (e) {
    }
  });
}
