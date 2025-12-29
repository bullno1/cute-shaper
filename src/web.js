addToLibrary({
	$web_init__postset: 'web_init();',
	$web_init: () => {

		_web_open_file = (filter, name_ptr, content_ptr, size_ptr) => {
			const input = document.createElement('input');
			input.type = 'file';

			return Asyncify.handleAsync(async () => {
				input.accept = UTF8ToString(filter);
				input.click();

				const files = await new Promise((resolve) => {
					input.addEventListener("change", (event) => {
						resolve(Array.from(input.files));
					}, {once: true});

					document.body.addEventListener("focus", (event) => {
						console.log("Return");
						resolve([]);
					}, {once: true});
				});

				if (files.length === 1) {
					const file = files[0];

					const nameLength = lengthBytesUTF8(file.name);
					const name = _malloc(nameLength + 1);
					stringToUTF8(file.name, name, nameLength + 1);

					const buffer = new Uint8Array(await file.arrayBuffer());
					const content = _malloc(buffer.byteLength);
					HEAPU8.set(buffer, content);

					setValue(name_ptr, name, 'i32');
					setValue(content_ptr, content, 'i32');
					setValue(size_ptr, buffer.byteLength, 'i32');
					return 1;
				} else {
					setValue(name_ptr, 0, 'i32');
					setValue(content_ptr, 0, 'i32');
					setValue(size_ptr, 0, 'i32');
					return 0;
				}
			});
		}

		_save_into_file = (path, data, size) => {
			return false;
		}
	},
	web_open_file: () => {},
	web_open_file__deps: ['$web_init'],
	save_into_file: () => {},
	save_into_file__deps: ['$web_init'],
});
