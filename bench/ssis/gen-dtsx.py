#!/usr/bin/env python3
"""Generate runnable .dtsx files for the betl/SSIS bench comparison.

Three shapes:
  A-1col            OLE DB Source (1 BIGINT)  → RowCount
  A-10col           OLE DB Source (10 BIGINT) → RowCount
  B-derived-10col   OLE DB Source (10 BIGINT) → DerivedColumn (a2..j2 = +1)
                    → RowCount

Output dir: /workspace/betl/bench/ssis/packages/
"""
import os, sys, textwrap

OUT = "/workspace/betl/bench/ssis/packages"

CONN_STR = (
    "Data Source=host.containers.internal,1433;Initial Catalog=betl_bench;"
    "User ID=sa;Password=DevP@ssw0rd!42;Provider=MSOLEDBSQL;"
    "Persist Security Info=True;Trust Server Certificate=True;"
)


def header(name):
    return textwrap.dedent(f"""\
        <?xml version="1.0"?>
        <DTS:Executable
            xmlns:DTS="www.microsoft.com/SqlServer/Dts"
            DTS:refId="Package"
            DTS:CreationName="Microsoft.Package"
            DTS:DTSID="{{aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaa0001}}"
            DTS:ExecutableType="Microsoft.Package"
            DTS:LocaleID="1033"
            DTS:ObjectName="{name}"
            DTS:PackageType="5"
            DTS:VersionBuild="0"
            DTS:VersionGUID="{{bbbbbbbb-bbbb-bbbb-bbbb-bbbbbbbb0001}}">
          <DTS:ConnectionManagers>
            <DTS:ConnectionManager
                DTS:refId="Package.ConnectionManagers[SrcDb]"
                DTS:CreationName="OLEDB"
                DTS:DTSID="{{cccccccc-cccc-cccc-cccc-cccccccc0001}}"
                DTS:ObjectName="SrcDb">
              <DTS:ObjectData>
                <DTS:ConnectionManager DTS:ConnectionString="{CONN_STR}"/>
              </DTS:ObjectData>
            </DTS:ConnectionManager>
          </DTS:ConnectionManagers>
          <DTS:Variables>
            <DTS:Variable
                DTS:CreationName="User::RowCount"
                DTS:DTSID="{{dddddddd-dddd-dddd-dddd-dddddddd0001}}"
                DTS:Namespace="User"
                DTS:ObjectName="RowCount">
              <DTS:VariableValue DTS:DataType="3">0</DTS:VariableValue>
            </DTS:Variable>
          </DTS:Variables>
        """)


def footer():
    return textwrap.dedent("""\
        </DTS:Executable>
        """)


def ole_source(cols, table):
    cols_list = ",".join(cols)
    sql = f"SELECT {cols_list} FROM dbo.{table}"

    out_cols = "\n".join(
        f'''                    <outputColumn
                        refId="Package\\DataFlow\\OleSource.Outputs[OLE DB Source Output].Columns[{c}]"
                        dataType="i8"
                        errorOrTruncationOperation="Conversion"
                        errorRowDisposition="FailComponent"
                        externalMetadataColumnId="Package\\DataFlow\\OleSource.Outputs[OLE DB Source Output].ExternalColumns[{c}]"
                        lineageId="Package\\DataFlow\\OleSource.Outputs[OLE DB Source Output].Columns[{c}]"
                        name="{c}"
                        truncationRowDisposition="FailComponent"/>'''
        for c in cols)
    ext_cols = "\n".join(
        f'''                    <externalMetadataColumn
                        refId="Package\\DataFlow\\OleSource.Outputs[OLE DB Source Output].ExternalColumns[{c}]"
                        dataType="i8"
                        name="{c}"/>'''
        for c in cols)
    err_cols = "\n".join(
        f'''                    <outputColumn
                        refId="Package\\DataFlow\\OleSource.Outputs[OLE DB Source Error Output].Columns[{c}]"
                        dataType="i8"
                        lineageId="Package\\DataFlow\\OleSource.Outputs[OLE DB Source Error Output].Columns[{c}]"
                        name="{c}"/>'''
        for c in cols)
    err_cols += '''
                    <outputColumn
                        refId="Package\\DataFlow\\OleSource.Outputs[OLE DB Source Error Output].Columns[ErrorCode]"
                        dataType="i4"
                        lineageId="Package\\DataFlow\\OleSource.Outputs[OLE DB Source Error Output].Columns[ErrorCode]"
                        name="ErrorCode"
                        specialFlags="1"/>
                    <outputColumn
                        refId="Package\\DataFlow\\OleSource.Outputs[OLE DB Source Error Output].Columns[ErrorColumn]"
                        dataType="i4"
                        lineageId="Package\\DataFlow\\OleSource.Outputs[OLE DB Source Error Output].Columns[ErrorColumn]"
                        name="ErrorColumn"
                        specialFlags="2"/>'''

    return f'''            <component
                refId="Package\\DataFlow\\OleSource"
                componentClassID="Microsoft.OLEDBSource"
                contactInfo=""
                description="OLE DB Source"
                name="OleSource"
                usesDispositions="true"
                version="7">
              <properties>
                <property dataType="System.Int32" name="AccessMode">2</property>
                <property dataType="System.String" name="OpenRowset"></property>
                <property dataType="System.String" name="OpenRowsetVariable"></property>
                <property dataType="System.String" name="SqlCommand">{sql}</property>
                <property dataType="System.String" name="SqlCommandVariable"></property>
                <property dataType="System.Int32" name="CommandTimeout">0</property>
                <property dataType="System.Int32" name="DefaultCodePage">1252</property>
                <property dataType="System.Boolean" name="AlwaysUseDefaultCodePage">false</property>
                <property dataType="System.String" name="ParameterMapping"></property>
              </properties>
              <connections>
                <connection
                    refId="Package\\DataFlow\\OleSource.Connections[OleDbConnection]"
                    connectionManagerID="Package.ConnectionManagers[SrcDb]"
                    connectionManagerRefId="Package.ConnectionManagers[SrcDb]"
                    description="The OLE DB runtime connection used to access the database."
                    name="OleDbConnection"/>
              </connections>
              <outputs>
                <output
                    refId="Package\\DataFlow\\OleSource.Outputs[OLE DB Source Output]"
                    name="OLE DB Source Output">
                  <outputColumns>
{out_cols}
                  </outputColumns>
                  <externalMetadataColumns isUsed="True">
{ext_cols}
                  </externalMetadataColumns>
                </output>
                <output
                    refId="Package\\DataFlow\\OleSource.Outputs[OLE DB Source Error Output]"
                    isErrorOut="true"
                    name="OLE DB Source Error Output">
                  <outputColumns>
{err_cols}
                  </outputColumns>
                  <externalMetadataColumns/>
                </output>
              </outputs>
            </component>
'''


def row_count(input_cols, upstream_output):
    """RowCount component with HasSideEffects=true so SSIS doesn't prune it.
    input_cols are tuples (col_name, lineage_output_refid)."""
    in_cols = "\n".join(
        f'''                    <inputColumn
                        refId="Package\\DataFlow\\RowCount.Inputs[Row Count Input 1].Columns[{c}]"
                        cachedDataType="i8"
                        cachedName="{c}"
                        lineageId="{lineage}"/>'''
        for c, lineage in input_cols)
    return f'''            <component
                refId="Package\\DataFlow\\RowCount"
                componentClassID="Microsoft.RowCount"
                contactInfo=""
                description="Row Count"
                name="RowCount"
                usesDispositions="true">
              <properties>
                <property dataType="System.String" name="VariableName">User::RowCount</property>
              </properties>
              <inputs>
                <input
                    refId="Package\\DataFlow\\RowCount.Inputs[Row Count Input 1]"
                    hasSideEffects="true"
                    name="Row Count Input 1">
                  <inputColumns>
{in_cols}
                  </inputColumns>
                  <externalMetadataColumns/>
                </input>
              </inputs>
              <outputs>
                <output
                    refId="Package\\DataFlow\\RowCount.Outputs[Row Count Output 1]"
                    name="Row Count Output 1"
                    synchronousInputId="Package\\DataFlow\\RowCount.Inputs[Row Count Input 1]"/>
              </outputs>
            </component>
'''


def derived_column(cols, upstream_output_ref):
    """Derived Column: for each col c, add c2 = [c] + 1.
    Has 2 outputs (main + error) per SSIS contract."""
    in_cols = "\n".join(
        f'''                    <inputColumn
                        refId="Package\\DataFlow\\Derived.Inputs[Derived Column Input].Columns[{c}]"
                        cachedDataType="i8"
                        cachedName="{c}"
                        lineageId="Package\\DataFlow\\OleSource.Outputs[OLE DB Source Output].Columns[{c}]"/>'''
        for c in cols)

    new_cols = []
    for c in cols:
        new_cols.append(f'''                    <outputColumn
                        refId="Package\\DataFlow\\Derived.Outputs[Derived Column Output].Columns[{c}2]"
                        dataType="i8"
                        errorOrTruncationOperation="Computation"
                        errorRowDisposition="FailComponent"
                        lineageId="Package\\DataFlow\\Derived.Outputs[Derived Column Output].Columns[{c}2]"
                        name="{c}2"
                        truncationRowDisposition="FailComponent">
                      <properties>
                        <property dataType="System.String" name="Expression">[{c}] + (DT_I8)1</property>
                        <property dataType="System.String" name="FriendlyExpression">[{c}] + (DT_I8)1</property>
                      </properties>
                    </outputColumn>''')
    out_cols_xml = "\n".join(new_cols)

    return f'''            <component
                refId="Package\\DataFlow\\Derived"
                componentClassID="Microsoft.DerivedColumn"
                contactInfo=""
                description="Derived Column"
                name="Derived"
                usesDispositions="true">
              <inputs>
                <input
                    refId="Package\\DataFlow\\Derived.Inputs[Derived Column Input]"
                    name="Derived Column Input">
                  <inputColumns>
{in_cols}
                  </inputColumns>
                  <externalMetadataColumns/>
                </input>
              </inputs>
              <outputs>
                <output
                    refId="Package\\DataFlow\\Derived.Outputs[Derived Column Output]"
                    name="Derived Column Output"
                    synchronousInputId="Package\\DataFlow\\Derived.Inputs[Derived Column Input]"
                    exclusionGroup="1">
                  <outputColumns>
{out_cols_xml}
                  </outputColumns>
                  <externalMetadataColumns/>
                </output>
                <output
                    refId="Package\\DataFlow\\Derived.Outputs[Derived Column Error Output]"
                    name="Derived Column Error Output"
                    synchronousInputId="Package\\DataFlow\\Derived.Inputs[Derived Column Input]"
                    isErrorOut="true"
                    exclusionGroup="1">
                  <outputColumns>
                    <outputColumn
                        refId="Package\\DataFlow\\Derived.Outputs[Derived Column Error Output].Columns[ErrorCode]"
                        dataType="i4"
                        lineageId="Package\\DataFlow\\Derived.Outputs[Derived Column Error Output].Columns[ErrorCode]"
                        name="ErrorCode"
                        specialFlags="1"/>
                    <outputColumn
                        refId="Package\\DataFlow\\Derived.Outputs[Derived Column Error Output].Columns[ErrorColumn]"
                        dataType="i4"
                        lineageId="Package\\DataFlow\\Derived.Outputs[Derived Column Error Output].Columns[ErrorColumn]"
                        name="ErrorColumn"
                        specialFlags="2"/>
                  </outputColumns>
                  <externalMetadataColumns/>
                </output>
              </outputs>
            </component>
'''


def package(name, cols, table, with_derived=False):
    """Compose a package: OleSource → [Derived] → RowCount."""
    src = ole_source(cols, table)

    if with_derived:
        # Cols flow into Derived, which produces a2..j2 via [c] + 1.
        # Row Count's input lineage points to the Derived Column Output.
        # But cols a..j flow through synchronously so their lineageId still
        # points to OleSource. The c2 cols come from Derived.
        dc = derived_column(cols, "Package\\DataFlow\\OleSource.Outputs[OLE DB Source Output]")
        # Row Count receives a..j (from source, sync-passed through derived)
        # plus a2..j2 (newly derived). Make the input ref each.
        in_pairs = [(c, f"Package\\DataFlow\\OleSource.Outputs[OLE DB Source Output].Columns[{c}]")
                    for c in cols]
        in_pairs += [(f"{c}2", f"Package\\DataFlow\\Derived.Outputs[Derived Column Output].Columns[{c}2]")
                     for c in cols]
        rc = row_count(in_pairs, None)
        paths = '''            <path
                refId="Package\\DataFlow.Paths[SrcToDerived]"
                endId="Package\\DataFlow\\Derived.Inputs[Derived Column Input]"
                name="SrcToDerived"
                startId="Package\\DataFlow\\OleSource.Outputs[OLE DB Source Output]"/>
            <path
                refId="Package\\DataFlow.Paths[DerivedToCount]"
                endId="Package\\DataFlow\\RowCount.Inputs[Row Count Input 1]"
                name="DerivedToCount"
                startId="Package\\DataFlow\\Derived.Outputs[Derived Column Output]"/>'''
        components = src + dc + rc
    else:
        in_pairs = [(c, f"Package\\DataFlow\\OleSource.Outputs[OLE DB Source Output].Columns[{c}]")
                    for c in cols]
        rc = row_count(in_pairs, None)
        paths = '''            <path
                refId="Package\\DataFlow.Paths[SrcToCount]"
                endId="Package\\DataFlow\\RowCount.Inputs[Row Count Input 1]"
                name="SrcToCount"
                startId="Package\\DataFlow\\OleSource.Outputs[OLE DB Source Output]"/>'''
        components = src + rc

    pipeline = f'''      <DTS:ObjectData>
        <pipeline DefaultBufferMaxRows="10000" DefaultBufferSize="10485760" version="1">
          <components>
{components}          </components>
          <paths>
{paths}
          </paths>
        </pipeline>
      </DTS:ObjectData>
'''

    dataflow = f'''  <DTS:Executables>
    <DTS:Executable
        DTS:refId="Package\\DataFlow"
        DTS:CreationName="Microsoft.Pipeline"
        DTS:DTSID="{{eeeeeeee-eeee-eeee-eeee-eeeeeeee0001}}"
        DTS:ExecutableType="Microsoft.Pipeline"
        DTS:ObjectName="DataFlow">
      <DTS:Variables/>
{pipeline}    </DTS:Executable>
  </DTS:Executables>
'''
    return header(name) + dataflow + footer()


def main():
    pkgs = [
        ("A_1col",             ["a"],                "src_1col",     False),
        ("A_10col",            list("abcdefghij"),   "src_10col",    False),
        ("B_derived_10col",    list("abcdefghij"),   "src_10col",    True),
        # 1M-row variant — startup tax becomes <10% of wall, exposes
        # steady-state data-flow throughput rather than process launch.
        ("B_derived_10col_1m", list("abcdefghij"),   "src_10col_1m", True),
    ]
    for name, cols, table, with_derived in pkgs:
        s = package(name, cols, table, with_derived)
        path = os.path.join(OUT, name.replace("_", "-") + ".dtsx")
        with open(path, "w") as f:
            f.write(s)
        print(f"wrote {path}  ({len(s)} bytes)")


if __name__ == "__main__":
    main()
