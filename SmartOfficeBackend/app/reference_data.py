"""Canonical Smart Office reference data shared by setup and validation."""

COMPANIES = (
    (1, "Company A"),
    (2, "Company B"),
    (3, "IT"),
    (4, "Management"),
    (5, "Company C"),
    (6, "Company D"),
)

USERS = (
    (1, 1, "Employee A", "Employee", 1, True),
    (2, 2, "Employee B", "Employee", 2, True),
    (3, 5, "Employee C", "Employee", 3, True),
    (4, 6, "Employee D", "Employee", 4, True),
    (5, 3, "IT Admin", "IT", 5, True),
    (6, 4, "Manager", "Manager", 6, True),
)

AREAS = (
    (1, "Company A", True),
    (2, "Company B", True),
    (3, "Company C", True),
    (4, "Company D", True),
    (5, "Server Room", True),
    (6, "Management / Admin", True),
    (7, "Main Entrance", True),
)

ALLOWED_AREA_IDS = {
    1: frozenset({1, 7}),
    2: frozenset({2, 7}),
    3: frozenset({3, 7}),
    4: frozenset({4, 7}),
    5: frozenset({5, 7}),
    6: frozenset({1, 2, 3, 4, 5, 6, 7}),
}

PERMISSIONS = tuple(
    (user_id, area_id, area_id in ALLOWED_AREA_IDS[user_id])
    for user_id in range(1, 7)
    for area_id in range(1, 8)
)

STATUS_KEYS = tuple(
    (user_id, area_id)
    for user_id in range(1, 7)
    for area_id in range(1, 8)
)

APPLICATION_TABLES = frozenset(
    {
        "Companies",
        "Users",
        "Areas",
        "Permissions",
        "UserAreaStatus",
        "AccessLogs",
        "SystemState",
    }
)
