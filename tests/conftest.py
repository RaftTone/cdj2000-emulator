import pytest


@pytest.fixture(autouse=True)
def _no_parent_watch(request, monkeypatch):
    # Many launcher tests stub time.sleep; the parent-poll thread would then
    # spin and record its poll as a launcher sleep. The watchdog's own test
    # needs the real behaviour.
    if request.module.__name__.rsplit('.', 1)[-1] != 'test_orphan_watchdog':
        monkeypatch.setenv('CDJ_NO_PARENT_WATCH', '1')
