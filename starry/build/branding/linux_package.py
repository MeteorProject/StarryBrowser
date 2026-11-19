# Copyright 2026 The Starry Browser Authors

"""Starry additions to Chromium's Linux package configuration."""


def configure_package(config):
    if 'PRODUCT_ID' not in config.branding_vars:
        return
    # Until Starry has its own repository, packages must not enroll in an
    # upstream repository or install upstream signing keys.
    config.repoconfig = ''
    config.repoconfigregex = ''
    config.appstream_screenshot_url = ''


def configure_flatpak(config):
    config.developer_id = config.branding_vars['APPLICATION_ID'].rsplit('.', 1)[0]
    config.project_license_spdx = config.info_vars['PROJECT_LICENSE']
    config.flatpak_command = config.branding_vars['PRODUCT_ID']
    config.flatpak_tags = ''
    return config.branding_vars['APPLICATION_ID']


def direct_launch_scheme(config):
    if config.channel == 'stable':
        return f"x-scheme-handler/{config.branding_vars['PRODUCT_ID']};"
    return ''
