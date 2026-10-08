# Автогонки (вариант 20)

Последовательная имитация кольцевой гонки по правилам В. Пудова.
Консольная программа на C (libc + системные вызовы `write`, `open`, `close`).

## Сборка

```bash
make
```

## Запуск

```bash
./autogonki
./autogonki --help
./autogonki --interactive
./autogonki --teams 3 --cars 2 --laps 2 --seed 1 --delay 80 --log race.log
```

Машины обозначаются буквами `A`, `B`, `C`, … Команда машины: `id / cars_per_team`.

## Тесты

```bash
make test
```
