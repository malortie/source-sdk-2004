# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

- Initial release
- All
  - concommand `dumpentityfactories`
  - concommand `givecurrentammo`
  - Sample mod files

### Fixed

- HL2
  - Fix projects file name in Game_HL2.sln
  - Set correct field name in Manhack send table
  - Wrong class used in CNewRecharge SetThink
  - Jeep eye position
  - Gravity string mismatch between client and server
  - Wrong class used for file system interface
  - Undefined behavior with null max clip1 size
  - Crash when `vgui::localize()` returns null
