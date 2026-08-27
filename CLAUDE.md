# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## 가장 먼저 할 일: AGENTS.md를 읽으세요

이 저장소의 프로젝트 목적, 면책 사항, 절대 금지 사항, 기술 스택, 아키텍처, 기능 스펙은 전부
**`AGENTS.md`**에 정의되어 있습니다. `AGENTS.md`가 본체 문서이며, 이 파일은 그것을 대체하지 않습니다.
작업을 시작하기 전에 반드시 `AGENTS.md`를 전체 읽으세요. 내용이 이 파일과 충돌하면 `AGENTS.md`가
우선합니다 (아래 Claude 전용 항목 제외).

## Claude Code 전용 지침

- `AGENTS.md`의 금지 목록(BYOVD, 하이퍼바이저 은폐/우회, 커널 드라이버·안티치트 우회 목적 코드)은
  Claude Code의 자체 판단으로도 절대 예외를 두지 마세요. 리버싱/후킹 작업 중 "이게 있으면 더 안정적이지
  않을까" 같은 이유로 스스로 스코프를 넓히지 말고, 그런 필요가 느껴지면 코드를 작성하는 대신 사용자에게
  먼저 확인하세요.
