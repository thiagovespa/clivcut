# clivcut — atalhos de build.
#
#   make dmg           .dmg desta arquitetura (Mac Intel -> x86_64), no Docker
#   make dmg-all       .dmg x86_64 e arm64
#   make help          lista os alvos
#
# O .dmg sai de um builder em Docker (packaging/macos/Dockerfile) que faz
# cross-compile para macOS com o SDK deste Mac: além do Docker, nada é
# instalado no host. O build nativo continua sendo o do README (CMake).
# Compatível com o GNU Make 3.81 do macOS.

# O Docker Desktop nem sempre põe o CLI no PATH.
export PATH := $(PATH):/Applications/Docker.app/Contents/Resources/bin

DOCKER       ?= docker
BUILDER      ?= clivcut-macos-builder
# Arquiteturas do `make dmg`: x86_64 | arm64 (separadas por espaço).
ARCHS        ?= $(shell uname -m)
# Volume Docker com as pastas de build (rebuild incremental, fora do repositório).
CACHE_VOLUME ?= clivcut-macos-cache
# 1 = usa a imagem do builder que já existe (a CI a cria antes, com cache).
SKIP_BUILDER ?=
SDK_DIR      := .cache/macos-sdk
ART_OUT      ?= .cache/dmg-art

BUILDER_RUN = $(DOCKER) run --rm -v "$(CURDIR)":/src -v $(CACHE_VOLUME):/cache

.PHONY: help macos-sdk builder dmg dmg-all dmg-art macos-shell clean clean-all

help:			## Mostra esta ajuda
	@grep -E '^[a-zA-Z_-]+:.*## ' $(MAKEFILE_LIST) | sed -e 's/:[^#]*## */|/' \
		| awk -F'|' '{ printf "  make %-12s %s\n", $$1, $$2 }'
	@echo
	@echo "Variáveis: ARCHS=\"x86_64 arm64\"  SKIP_BUILDER=1"

$(SDK_DIR)/MacOSX.sdk.tar.gz:
	sh packaging/macos/export-sdk.sh $(SDK_DIR)

macos-sdk: $(SDK_DIR)/MacOSX.sdk.tar.gz	## Exporta o SDK do macOS deste Mac para o builder

builder: $(SDK_DIR)/MacOSX.sdk.tar.gz	## Cria/atualiza a imagem do builder macOS
	@if [ -n "$(SKIP_BUILDER)" ]; then echo "SKIP_BUILDER: usando a imagem $(BUILDER) existente"; \
	else $(DOCKER) build -t $(BUILDER) --build-context sdk=$(SDK_DIR) \
		-f packaging/macos/Dockerfile packaging/macos; fi

dmg: builder		## .dmg de ARCHS (padrão: esta máquina) em dist/
	$(BUILDER_RUN) $(BUILDER) $(ARCHS)

dmg-all: builder	## .dmg x86_64 e arm64 em dist/
	$(BUILDER_RUN) $(BUILDER) x86_64 arm64

dmg-art: builder	## Prévia do fundo e dos ícones do DMG em ART_OUT
	$(BUILDER_RUN) --entrypoint python3 $(BUILDER) packaging/macos/dmg.py --preview $(ART_OUT)

macos-shell: builder	## Shell no builder (depuração)
	$(BUILDER_RUN) -it --entrypoint bash $(BUILDER)

clean:			## Apaga dist/ e o cache de build do Docker
	rm -rf dist $(ART_OUT)
	-$(DOCKER) volume rm $(CACHE_VOLUME)

clean-all: clean	## clean + a imagem do builder e o SDK exportado
	-$(DOCKER) image rm $(BUILDER)
	rm -rf $(SDK_DIR)
