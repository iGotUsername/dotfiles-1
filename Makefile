.DEFAULT_GOAL := help

help:
	@printf '%s\n' 'make plan       Preview full installation (no writes/network)' \
	  'make check      Check build dependencies' \
	  'make install    Build/install all components with confirmation and backups' \
	  'make test       Installer tests and everbar tests' \
	  'make clean      Remove local everbar build artifacts only'

plan:
	python3 scripts/install.py --dry-run

check:
	python3 scripts/install.py --check

install:
	python3 scripts/install.py

test-installer:
	python3 -m unittest discover -s tests -p 'test_installer.py' -v

test: test-installer
	$(MAKE) -C src/everbar test

clean:
	$(MAKE) -C src/everbar clean

.PHONY: help plan check install test-installer test clean
