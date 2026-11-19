# Copyright 2026 The Starry Browser Authors

"""Starry additions to Chromium's Linux package configuration."""

from pathlib import Path

from starry.build import version as starry_version


def configure_package(config):
    if 'PRODUCT_ID' not in config.branding_vars:
        return
    # Use Starry's own SemVer package version instead of Chromium's version.
    version = starry_version.read_version(
        Path(__file__).resolve().parents[2] / 'VERSION'
    )
    config.version = version.linux_package_version
    config.versionfull = f'{config.version}-{config.package_release}'
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
