import bloom
assert bloom.app.headless
with bloom.transactions.group('Embedded transaction'):
    bloom.ops.project.set_name(name='Embedded')
assert bloom.context.project.name == 'Embedded'
print('embedded Python passed')
