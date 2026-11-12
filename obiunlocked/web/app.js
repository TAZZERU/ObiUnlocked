// ObiUnlocked shared app bar: navigation, status, language, reboot, logout. Every page except /login loads it.
(function () {
  var lang = localStorage.getItem('lang') || 'de';
  var T = {
    de: { strom: 'Stromzähler', meine: 'Meine Geräte', funk: 'In der Nähe', empfang: 'Empfang', history: 'Verlauf', radio: 'OBI-Funk',
          settings: 'Einstellungen', debug: 'Debug', hilfe: 'Hilfe', reboot: 'Neustart', logout: 'Abmelden', menu: 'Menü',
          rebootq: 'Gateway jetzt neu starten?', wifi: 'WLAN', off: 'offline', dis: 'aus' },
    en: { strom: 'Power meters', meine: 'My devices', funk: 'Nearby', empfang: 'Reception', history: 'History', radio: 'OBI radio',
          settings: 'Settings', debug: 'Debug', hilfe: 'Help', reboot: 'Reboot', logout: 'Log out', menu: 'Menu',
          rebootq: 'Restart the gateway now?', wifi: 'WiFi', off: 'offline', dis: 'off' } };
  var L = T[lang] || T.de;
  var items = [['strom', '/#strom'], ['meine', '/#meine'], ['funk', '/#funk'], ['empfang', '/#empfang'], ['history', '/history'],
               ['radio', '/radio'], ['settings', '/settings'], ['debug', '/debug'], ['hilfe', '/#hilfe']];
  var esc = function (s) { return String(s == null ? '' : s).replace(/[&<>"]/g, function (c) { return { '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c]; }); };
  var _f = window.fetch;
  window.fetch = function () { return _f.apply(this, arguments).then(function (r) { if (r.status == 401) location.href = '/login'; return r; }); };

  var bar = document.createElement('header');
  bar.className = 'appbar';
  bar.innerHTML = '<div class="in"><a class="brand" href="/">ObiUnlocked<small id="ab_gw"></small></a>' +
    '<nav>' + items.map(function (i) { return '<a href="' + i[1] + '" data-p="' + i[0] + '">' + L[i[0]] + '</a>'; }).join('') + '</nav>' +
    '<span class="sp"></span><span class="st" id="ab_st"></span>' +
    '<span class="act"><span class="seg lang"><button data-l="de">DE</button><button data-l="en">EN</button></span>' +
    '<button class="btn ghost sm" id="ab_reboot">' + L.reboot + '</button><button class="btn ghost sm" id="ab_logout" hidden>' + L.logout + '</button></span>' +
    '<button class="btn sm menu" id="ab_menu">' + L.menu + '</button></div>';
  document.body.insertBefore(bar, document.body.firstChild);

  window.appNav = function (p) {
    [].forEach.call(bar.querySelectorAll('nav a'), function (a) { a.classList.toggle('cur', a.dataset.p === p); });
    bar.classList.remove('open');
  };
  window.appNav(document.body.dataset.page || '');
  [].forEach.call(bar.querySelectorAll('.seg button'), function (b) {
    b.classList.toggle('cur', b.dataset.l === lang);
    b.onclick = function () { localStorage.setItem('lang', b.dataset.l); location.reload(); };
  });
  bar.querySelector('#ab_menu').onclick = function () { bar.classList.toggle('open'); };
  bar.querySelector('#ab_reboot').onclick = function () { if (confirm(L.rebootq)) fetch('/api/reboot', { method: 'POST' }).catch(function () {}); };
  bar.querySelector('#ab_logout').onclick = function () { fetch('/api/logout', { method: 'POST' }).finally(function () { location.href = '/login'; }); };

  // status line; the dashboard feeds it from its own poll, other pages poll here
  window.appStatus = function (st) {
    var q = st.mqtt || {};
    bar.querySelector('#ab_gw').textContent = st.gw_custom_name || ((st.gw || st.gwid || '') + '').toUpperCase();
    bar.querySelector('#ab_logout').hidden = !(st.auth && st.auth.enabled);
    bar.querySelector('#ab_st').innerHTML =
      '<span><i class="dot ' + (st.wifi ? 'on' : 'off') + '"></i>' + L.wifi + ' ' + (st.wifi ? st.wifi_rssi + ' dBm' : L.off) + '</span>' +
      '<span><i class="dot ' + (!q.enabled ? '' : q.connected ? 'on' : 'off') + '"></i>MQTT ' + (!q.enabled ? L.dis : q.connected ? '' : esc(q.state)) + '</span>' +
      (st.temp_c != null ? '<span>' + st.temp_c + ' °C</span>' : '');
  };
  function poll() { fetch('/api/status').then(function (r) { return r.json(); }).then(window.appStatus).catch(function () {}); }
  if (!document.body.dataset.ownStatus) { poll(); setInterval(poll, 6000); }
})();
