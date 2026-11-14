// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Kleinia Authors
// prefs.js — SchedMgr 扩展偏好设置（libadwaita 风格）
//
// 仅一项设置：DBus bus-type（system 生产 / session 开发联调 mock-schedmgrd）。

import Adw from 'gi://Adw';
import Gtk from 'gi://Gtk';
import GLib from 'gi://GLib';

import {ExtensionPreferences} from 'resource:///org/gnome/Shell/Extensions/js/extensions/prefs.js';

const BUS_CHOICES = ['system', 'session'];

export default class SchedMgrPreferences extends ExtensionPreferences {
    fillPreferencesWindow(window) {
        const settings = this.getSettings();

        const page = new Adw.PreferencesPage({
            title: 'SchedMgr',
            icon_name: 'preferences-system-symbolic',
        });

        const busGroup = new Adw.PreferencesGroup({
            title: 'Connection',
            description: 'Where to reach the schedmgr daemon (io.kleinia.SchedMgr)',
        });
        page.add(busGroup);

        const busRow = new Adw.ComboRow({
            title: 'DBus Bus',
            subtitle: 'Use the session bus to develop against mock-schedmgrd; changes apply immediately',
            model: new Gtk.StringList({strings: BUS_CHOICES}),
        });
        const initial = BUS_CHOICES.indexOf(settings.get_string('bus-type'));
        busRow.selected = initial >= 0 ? initial : 0;
        busRow.connect('notify::selected', () => {
            const value = BUS_CHOICES[busRow.selected];
            if (settings.get_string('bus-type') !== value)
                settings.set_string('bus-type', value);
        });
        settings.connect('changed::bus-type', () => {
            const idx = BUS_CHOICES.indexOf(settings.get_string('bus-type'));
            if (idx >= 0 && busRow.selected !== idx)
                busRow.selected = idx;
        });
        busGroup.add(busRow);

        const aboutGroup = new Adw.PreferencesGroup({title: 'About'});
        aboutGroup.add(new Adw.ActionRow({
            title: 'Extension',
            subtitle: 'schedmgr@kleinia.io — Kleinia policy manager',
        }));
        aboutGroup.add(new Adw.ActionRow({
            title: 'Daemon',
            subtitle: 'io.kleinia.SchedMgr on the selected bus',
        }));
        aboutGroup.add(new Adw.ActionRow({
            title: 'API version',
            subtitle: 'io.kleinia.Manager1 / Sched1 / Io1, v1',
        }));
        page.add(aboutGroup);

        window.add(page);
    }
}
