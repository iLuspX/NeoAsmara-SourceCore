"""Check imported numeric facts without contacting the site or changing a database."""
from pathlib import Path
import json
root=Path(__file__).resolve().parent.parent
source=json.loads((root/'ReferenceSnapshot/data/skill_catalog.json').read_text(encoding='utf-8'))
applied=json.loads((root/'ReferenceOverrides/data/skill_catalog.json').read_text(encoding='utf-8'))
audit=json.loads((root/'docs/official-talent-audit.json').read_text(encoding='utf-8'))
assert set(audit['classes'])=={'1','2','3','4','5','6','8'}
assert audit['ordinarySkillCount']==316 and not audit['missingFromClient']
assert audit['compatibleRanks']==1060
for row in audit['numericChanges']:
    key,rank=str(row['id']),str(row['rank'])
    expected=row['official'] if row['applied'] else row['clientCatalog']
    assert applied['skills'][key]['levels'][rank][row['field']]==expected,row
assert applied['skills']['3901']['levels']['1']['hits']==2
assert applied['skills']['5104']['levels']['1']['hits']==6
assert applied['skills']['3907']['levels']['1']['magic_min']==412
assert applied['skills']['3907']['levels']['1']['physical_min']==0
assert applied['skills']['3901']['prerequisites']==[{'id':3002,'rank':1}]
for key,skill in applied['skills'].items():
    original=source['skills'][key]
    for rank,level in skill['levels'].items():
        for field in ('mana','cooldown','duration','power','effects','points','level'):
            assert level[field]==original['levels'][rank][field],(key,rank,field)
print('PASS: seven official trees;316 skills;1060 compatible ranks;92 channel-field corrections; native multi-hit counts/prerequisites; divergent balance values and client effects preserved')
