from .config_entry import ConfigEntry


class String(ConfigEntry):
    def _parse_from_text(self, text):
        self._value = text.strip('"')

    def _serialize_to_text(self):
        return f'"{self._value}"'

    def _set_value(self, value):
        if type(value) is not str:
            raise TypeError(f'str was expected, but {value} with type {type(value)} was passed')

        self._value = value